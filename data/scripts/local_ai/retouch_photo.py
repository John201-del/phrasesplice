"""Retouch a still photo locally and write a lossless PNG."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import stat
import sys

os.environ["OPENCV_IO_MAX_IMAGE_PIXELS"] = "24000000"
os.environ["OPENCV_IO_MAX_IMAGE_WIDTH"] = "12000"
os.environ["OPENCV_IO_MAX_IMAGE_HEIGHT"] = "12000"

import cv2
import numpy as np


class PhotoError(ValueError):
    pass


def workspace_path(value):
    configured = os.environ.get("LOCALCUT_WORKSPACE_ROOT", "")
    if not configured or not Path(configured).is_absolute():
        raise PhotoError("workspace_not_configured")
    root, path = Path(configured), Path(value)
    if not path.is_absolute():
        raise PhotoError("photo_output_conflict")
    for item in (root, *root.parents, path, *path.parents):
        if item.is_symlink() or item.exists() and getattr(item.lstat(), "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise PhotoError("photo_output_conflict")
    if not path.resolve().is_relative_to(root.resolve()):
        raise PhotoError("photo_output_conflict")
    return path.resolve()


def ordinary_file(name):
    path = Path(name)
    if not path.is_absolute() or not path.is_file():
        raise PhotoError("invalid_photo")
    for item in (path, *path.parents):
        info = item.lstat()
        if item.is_symlink() or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise PhotoError("invalid_photo")
    return path.resolve(strict=True)


def load_photo(path):
    path = ordinary_file(path)
    suffix = path.suffix.lower()
    if suffix not in (".jpg", ".jpeg", ".png") or path.stat().st_size > 100 * 1024**2:
        raise PhotoError("invalid_photo")
    data = np.fromfile(path, np.uint8)
    signature = bytes(data[:8])
    png = suffix == ".png"
    if (png and signature != b"\x89PNG\r\n\x1a\n") or (not png and not signature.startswith(b"\xff\xd8\xff")):
        raise PhotoError("invalid_photo")
    try:
        # JPEG's color decoder applies EXIF rotation; PNG's unchanged decoder retains alpha.
        photo = cv2.imdecode(data, cv2.IMREAD_UNCHANGED if png else cv2.IMREAD_COLOR)
    except cv2.error as error:
        raise PhotoError("invalid_photo") from error
    if photo is None or photo.dtype != np.uint8:
        raise PhotoError("invalid_photo")
    return cv2.cvtColor(photo, cv2.COLOR_GRAY2BGR) if photo.ndim == 2 else photo


def number(value, low, high):
    if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
        raise PhotoError("invalid_photo_options")
    return value


def repair_spots(image, spots):
    if not isinstance(spots, list) or not 1 <= len(spots) <= 12:
        raise PhotoError("invalid_photo_options")
    h, w = image.shape[:2]
    mask = np.zeros((h, w), np.uint8)
    for spot in spots:
        if not isinstance(spot, dict) or set(spot) != {"xPercent", "yPercent", "radiusPercent"}:
            raise PhotoError("invalid_photo_options")
        x = round(number(spot["xPercent"], 0, 100) * (w - 1) / 100)
        y = round(number(spot["yPercent"], 0, 100) * (h - 1) / 100)
        radius = max(1, round(number(spot["radiusPercent"], .2, 5) * min(w, h) / 100))
        cv2.circle(mask, (x, y), radius, 255, -1)
    return cv2.inpaint(image, mask, 3, cv2.INPAINT_TELEA)


def smooth_skin(image, strength):
    strength = number(strength, 1, 100) / 100
    detector = cv2.CascadeClassifier(str(Path(cv2.data.haarcascades) / "haarcascade_frontalface_default.xml"))
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    scale = min(1, 1000 / max(gray.shape))
    small = cv2.resize(gray, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA) if scale < 1 else gray
    faces = detector.detectMultiScale(small, 1.12, 5, minSize=(32, 32))
    if len(faces) == 0:
        raise PhotoError("no_face")
    for box in faces:
        x, y, w, h = [round(value / scale) for value in box]
        roi = image[y:y+h, x:x+w]
        h, w = roi.shape[:2]
        _, cr, cb = cv2.split(cv2.cvtColor(roi, cv2.COLOR_BGR2YCrCb))
        skin = (cr >= 132) & (cr <= 184) & (cb >= 74) & (cb <= 137)
        mask = np.zeros((h, w), np.uint8)
        cv2.ellipse(mask, (w//2, round(h*.55)), (round(w*.43), round(h*.43)), 0, 0, 360, 255, -1)
        # Keep the brows, eyes and mouth sharp, and feather the skin boundary.
        mask[round(h*.25):round(h*.51), round(w*.12):round(w*.88)] = 0
        mask[round(h*.70):round(h*.84), round(w*.26):round(w*.74)] = 0
        mask[~skin] = 0
        weight = cv2.GaussianBlur(mask, (0, 0), max(1, w*.017)).astype(np.float32)
        weight *= strength / 255
        softened = cv2.bilateralFilter(roi, min(13, 2*round(w*.016)+5), 26, max(3, w*.03))
        roi[:] = np.rint(roi + (softened.astype(np.float32) - roi) * weight[..., None]).astype(np.uint8)
    return image


def person_matte(image, models):
    import onnxruntime as ort
    ort.disable_telemetry_events()
    model = workspace_path(Path(models) / "rvm_mobilenetv3_fp32.onnx")
    if not model.is_file():
        raise PhotoError("missing_matte_model")
    options = ort.SessionOptions()
    options.intra_op_num_threads = 2
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(model), options, providers=["CPUExecutionProvider"])
    h, w = image.shape[:2]
    scale = min(1, 1280 / max(h, w))
    reduced = cv2.resize(image, (max(1, round(w*scale)), max(1, round(h*scale))), interpolation=cv2.INTER_AREA) if scale < 1 else image
    inputs = {"src": reduced[:, :, ::-1].transpose(2, 0, 1)[None].astype(np.float32) / 255,
              "downsample_ratio": np.array([min(1, 512 / max(reduced.shape[:2]))], np.float32)}
    inputs.update({f"r{i}i": np.zeros((1, 1, 1, 1), np.float32) for i in range(1, 5)})
    foreground, matte, *_ = session.run(None, inputs)
    alpha = np.clip(np.rint(matte[0, 0] * 255), 0, 255).astype(np.uint8)
    if np.count_nonzero(alpha >= 200) < max(4, alpha.size * .002):
        raise PhotoError("no_person")
    edges = np.clip(np.rint(foreground[0, ::-1].transpose(1, 2, 0) * 255), 0, 255).astype(np.uint8)
    if scale < 1:
        alpha = cv2.resize(alpha, (w, h), interpolation=cv2.INTER_LINEAR)
        edges = cv2.resize(edges, (w, h), interpolation=cv2.INTER_LINEAR)
    edges[alpha >= 250] = image[alpha >= 250]
    edges[alpha == 0] = 0
    return edges, alpha


def background_result(photo, original_alpha, config, models):
    if not isinstance(config, dict):
        raise PhotoError("invalid_photo_options")
    mode = config.get("mode")
    color = config.get("color")
    replacement = None
    if mode == "color" and set(config) == {"mode", "color"} and isinstance(color, str) and re.fullmatch(r"#[0-9a-fA-F]{6}", color):
        replacement = np.array([int(color[i:i+2], 16) for i in (5, 3, 1)], np.uint16)
    elif mode == "image" and set(config) == {"mode", "path"} and isinstance(config.get("path"), str):
        bg = load_photo(config["path"])
        if bg.shape[2] == 4:
            raise PhotoError("invalid_background_photo")
        h, w = photo.shape[:2]
        bh, bw = bg.shape[:2]
        cw, ch = max(1, min(bw, round(bh*w/h))), max(1, min(bh, round(bw*h/w)))
        x, y = (bw-cw)//2, (bh-ch)//2
        replacement = cv2.resize(bg[y:y+ch, x:x+cw], (w, h), interpolation=cv2.INTER_LINEAR).astype(np.uint16)
    elif mode != "transparent" or set(config) != {"mode"}:
        raise PhotoError("invalid_photo_options")
    foreground, alpha = person_matte(photo, models)
    if original_alpha is not None:
        alpha = np.minimum(original_alpha, alpha)
    if replacement is None:
        foreground[alpha == 0] = 0
        return np.dstack((foreground, alpha))
    weight = alpha[..., None].astype(np.uint16)
    # The two weights add to 255, so their sum fits in uint16.
    return ((foreground * weight + replacement * (255 - weight) + 127) // 255).astype(np.uint8)


class FaceCover:
    """Locate one visible face on every frame and cover it with a local image."""

    def __init__(self, sticker_path, video=False):
        self.sticker = load_photo(sticker_path)
        if self.sticker.shape[2] == 3:
            self.sticker = cv2.cvtColor(self.sticker, cv2.COLOR_BGR2BGRA)
        visible = self.sticker[:, :, 3] >= 160
        if not np.any(visible):
            raise PhotoError("invisible_sticker")
        ys, xs = np.where(visible)
        self.sticker = self.sticker[ys.min():ys.max()+1, xs.min():xs.max()+1]
        center = self.sticker[self.sticker.shape[0]//2, self.sticker.shape[1]//2]
        self.backing = tuple(map(int, center[:3])) if center[3] >= 160 else (46, 46, 46)
        self.detector = cv2.CascadeClassifier(str(Path(cv2.data.haarcascades) / "haarcascade_frontalface_default.xml"))
        self.previous = None
        self.video = video

    def apply(self, image):
        h, w = image.shape[:2]
        scale = min(1, 800 / max(h, w))
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        if scale < 1:
            gray = cv2.resize(gray, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
        boxes = self.detector.detectMultiScale(gray, 1.10, 5, minSize=(30, 30))
        if len(boxes) != 1:
            raise PhotoError("face_tracking_lost" if self.video else "single_face_required")
        x, y, fw, fh = (round(value / scale) for value in boxes[0])
        cx, cy = x + fw // 2, y + fh // 2
        ax, ay = cx, cy
        if self.previous is not None and math.dist((cx, cy), self.previous) < min(fw, fh) * .2:
            ax, ay = (round(.75 * current + .25 * last) for current, last in zip((cx, cy), self.previous))
        self.previous = (ax, ay)
        # An opaque backing covers the face even if the artwork has transparent holes.
        rx, ry = math.ceil(fw * .77), math.ceil(fh * .77)
        x0, y0, x1, y1 = max(0, cx-rx), max(0, cy-ry), min(w, cx+rx+1), min(h, cy+ry+1)
        cv2.ellipse(image[y0:y1, x0:x1], (cx-x0, cy-y0), (rx, ry), 0, 0, 360, self.backing, -1, cv2.LINE_AA)
        sh, sw = self.sticker.shape[:2]
        factor = max(fw, fh) * 1.75 / max(sw, sh)
        sw, sh = max(1, round(sw * factor)), max(1, round(sh * factor))
        sticker = cv2.resize(self.sticker, (sw, sh), interpolation=cv2.INTER_AREA if factor < 1 else cv2.INTER_LINEAR)
        x, y = ax - sw//2, ay - sh//2
        x0, y0, x1, y1 = max(0, x), max(0, y), min(w, x+sw), min(h, y+sh)
        overlay = sticker[y0-y:y1-y, x0-x:x1-x]
        roi = image[y0:y1, x0:x1]
        weight = overlay[:, :, 3:4].astype(np.uint16)
        roi[:] = (overlay[:, :, :3] * weight + roi * (255 - weight) + 127) // 255
        return image


def main():
    parser = argparse.ArgumentParser()
    for flag in ("input", "output", "model-root", "options"):
        parser.add_argument("--" + flag, required=True)
    args = parser.parse_args()
    cv2.setNumThreads(2)
    try:
        workspace_path(args.model_root)
        settings = json.loads(args.options)
        if not isinstance(settings, dict) or not settings or set(settings) - {"skinStrength", "blemishes", "background", "stickerPath"}:
            raise PhotoError("invalid_photo_options")
        if "stickerPath" in settings and (len(settings) != 1 or not isinstance(settings["stickerPath"], str)):
            raise PhotoError("invalid_photo_options")
        output = ordinary_file(workspace_path(args.output))
        if output.suffix.lower() != ".png" or output.stat().st_size:
            raise PhotoError("photo_output_conflict")
        source = load_photo(args.input)
        alpha = source[:, :, 3] if source.shape[2] == 4 else None
        photo = source[:, :, :3].copy()
        if "blemishes" in settings:
            photo = repair_spots(photo, settings["blemishes"])
        if "skinStrength" in settings:
            photo = smooth_skin(photo, settings["skinStrength"])
        if "stickerPath" in settings:
            photo = FaceCover(settings["stickerPath"]).apply(photo)
        if "background" in settings:
            photo = background_result(photo, alpha, settings["background"], args.model_root)
        elif alpha is not None:
            photo = np.dstack((photo, alpha))
        ok, encoded = cv2.imencode(".png", photo, [cv2.IMWRITE_PNG_COMPRESSION, 3])
        if not ok:
            raise PhotoError("photo_encoding_failed")
        with output.open("r+b") as file:
            if os.fstat(file.fileno()).st_size:
                raise PhotoError("photo_output_conflict")
            file.write(encoded.tobytes())
        h, w = photo.shape[:2]
        print(json.dumps({"ok": True, "path": str(output), "width": w, "height": h, "alpha": photo.shape[2] == 4}), flush=True)
        return 0
    except PhotoError as error:
        print(json.dumps({"error": str(error)}), flush=True)
    except (OSError, ValueError, cv2.error):
        print('{"error":"photo_processing_failed"}', flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
