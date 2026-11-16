"""Draw a shaded cartoon character driven by locally measured facial landmarks."""
import hashlib
import json
import math
from pathlib import Path

import cv2
import numpy as np


class PhotoError(ValueError):
    pass


class AnimatedFace:
    SIZE = 192

    def __init__(self, model_root):
        import torch
        from facexlib.alignment.awing_arch import FAN

        manifest = Path(__file__).resolve().parents[1] / "avatar/model_artifacts.json"
        name = "alignment_WFLW_4HG.pth"
        expected = next(item for item in json.loads(manifest.read_text(encoding="utf-8")) if item["name"] == name)
        model = Path(model_root) / name
        if not model.is_file() or model.stat().st_size != expected["bytes"]:
            raise PhotoError("animated_model_unavailable")
        with model.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != expected["sha256"]:
                raise PhotoError("animated_model_unavailable")
        self.torch = torch
        torch.set_num_threads(2)
        torch.set_num_interop_threads(1)
        self.landmarker = FAN(num_modules=4, num_landmarks=98, device="cpu")
        self.landmarker.load_state_dict(torch.load(str(model), map_location="cpu", weights_only=True)["state_dict"], strict=True)
        self.landmarker.eval()
        self.detector = cv2.CascadeClassifier(str(Path(cv2.data.haarcascades) / "haarcascade_frontalface_default.xml"))
        self.y, self.x = np.mgrid[:self.SIZE, :self.SIZE].astype(np.float32)

    def paint(self, color, alpha, center, radii, pigment):
        dx, dy = (self.x - center[0]) / radii[0], (self.y - center[1]) / radii[1]
        distance = dx * dx + dy * dy
        nz = np.sqrt(np.maximum(0, 1 - distance))
        light = np.clip(-.36 * dx - .47 * dy + .80 * nz, 0, 1)
        highlight = np.power(light, 14) * 58
        shade = np.clip(np.asarray(pigment) * (.43 + .57 * light[..., None]) + highlight[..., None], 0, 255)
        opacity = np.clip((1 - distance) * min(radii) / 2, 0, 1)
        weight = opacity[..., None]
        color[:] = shade * weight + color * (1 - weight)
        alpha[:] = opacity + alpha * (1 - opacity)

    def character(self, gap, mouth_width, yaw, roll, size):
        color = np.zeros((self.SIZE, self.SIZE, 3), np.float32)
        alpha = np.zeros(color.shape[:2], np.float32)
        shift = round(20 * yaw)
        self.paint(color, alpha, (96, 106), (77, 65), (82, 196, 60))
        for center in (53, 139):
            cx = center + shift
            self.paint(color, alpha, (cx, 58), (25, 26), (78, 198, 56))
            self.paint(color, alpha, (cx, 58), (17, 18), (212, 245, 231))
            self.paint(color, alpha, (cx + shift // 2, 60), (8, 10), (29, 52, 22))
        color = np.rint(color).astype(np.uint8)
        mx, my = 96 + shift, 126
        mw = round(np.clip(10 + 50 * mouth_width, 18, 44))
        mh = round(np.clip((gap - .014) * 135, 0, 24))
        ink = (48, 20, 76)
        if mh <= 2:
            cv2.ellipse(color, (mx, my-3), (mw, 5), 0, 12, 168, ink, 3, cv2.LINE_AA)
        else:
            cv2.ellipse(color, (mx, my), (mw, mh), 0, 0, 360, ink, -1, cv2.LINE_AA)
            if mh >= 7:
                cv2.ellipse(color, (mx, my+mh//2), (mw//2, max(2, mh//3)), 0, 0, 360, (101, 96, 196), -1, cv2.LINE_AA)
                cv2.ellipse(color, (mx, my-mh//2), (mw//2, 2), 0, 0, 360, (204, 236, 235), -1, cv2.LINE_AA)
        image = np.dstack((color, np.rint(alpha * 255).astype(np.uint8)))
        # Warp premultiplied color and alpha together to keep the character's edges clean.
        rotation = cv2.getRotationMatrix2D((96, 96), -roll, 1)
        image = cv2.warpAffine(image, rotation, (self.SIZE, self.SIZE), flags=cv2.INTER_LINEAR)
        width = round(size * (1 - min(.14, abs(yaw) * .28)))
        return cv2.resize(image, (width, size), interpolation=cv2.INTER_AREA if size < self.SIZE else cv2.INTER_LINEAR)

    def apply(self, image):
        h, w = image.shape[:2]
        scale = min(1, 800 / max(h, w))
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        if scale < 1:
            gray = cv2.resize(gray, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
        boxes = self.detector.detectMultiScale(gray, 1.1, 5, minSize=(40, 40))
        if len(boxes) != 1:
            raise PhotoError("face_tracking_lost")
        x, y, fw, fh = (round(value / scale) for value in boxes[0])
        try:
            with self.torch.inference_mode():
                points = self.landmarker.get_landmarks(image[y:y+fh, x:x+fw])
        except IndexError as error:
            raise PhotoError("face_tracking_lost") from error
        if points.shape != (98, 2) or not np.isfinite(points).all():
            raise PhotoError("face_tracking_lost")
        eye_left, eye_right = points[60:68].mean(axis=0), points[68:76].mean(axis=0)
        axis = eye_right - eye_left
        if float(np.linalg.norm(axis)) < fw * .12:
            raise PhotoError("face_tracking_lost")
        roll = float(np.clip(math.degrees(math.atan2(axis[1], axis[0])), -25, 25))
        yaw = float(np.clip((points[54, 0] - (eye_left[0]+eye_right[0])/2) / axis[0], -.44, .44)) if abs(axis[0]) > 1 else 0
        gap = float((points[93:96, 1].mean() - points[89:92, 1].mean()) / fh)
        mouth_width = float(np.linalg.norm(points[82] - points[76]) / fw)
        if mouth_width < .10 or mouth_width > .85:
            raise PhotoError("face_tracking_lost")
        size = round(max(fw, fh) * 1.93)
        character = self.character(gap, mouth_width, yaw, roll, size)
        sh, sw = character.shape[:2]
        sx, sy = x + fw//2 - sw//2, y + fh//2 - sh//2
        x0, y0, x1, y1 = max(0, sx), max(0, sy), min(w, sx+sw), min(h, sy+sh)
        tile = character[y0-sy:y1-sy, x0-sx:x1-sx]
        roi = image[y0:y1, x0:x1]
        weight = tile[:, :, 3:4].astype(np.uint16)
        roi[:] = np.minimum(tile[:, :, :3] + (roi * (255 - weight) + 127) // 255, 255).astype(np.uint8)
        return image
