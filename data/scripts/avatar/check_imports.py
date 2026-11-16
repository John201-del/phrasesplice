"""Dependency and isolation smoke test; deliberately requires no model weights."""
import json
from pathlib import Path
import sys
import os
from tempfile import TemporaryDirectory
sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_avatar import workspace_path

root = workspace_path(sys.argv[1])
for key, relative in (('MPLCONFIGDIR', 'cache/matplotlib'), ('JOBLIB_TEMP_FOLDER', 'cache/tmp'), ('NUMBA_CACHE_DIR', 'cache/numba')):
    directory = workspace_path(root / relative)
    directory.mkdir(parents=True, exist_ok=True)
    os.environ[key] = str(directory)
os.environ['MPLBACKEND'] = 'Agg'
source = root / 'source/SadTalker'
sys.path.insert(0, str(source))
assert all(Path(value).resolve().is_relative_to(root) or Path(value).resolve() == Path(__file__).resolve().parent for value in sys.path if value)
import torch
import torchvision
import numpy
import imageio
import scipy
import librosa
import skimage
import facexlib
import inference
assert torch.version.cuda is None
assert numpy.__version__ == '1.23.5'
assert torchvision.ops.nms(torch.tensor([[0., 0., 1., 1.]]), torch.tensor([1.]), .5).tolist() == [0]
with TemporaryDirectory(dir=root / 'tmp') as temp:
    output = Path(temp) / 'codec-probe.mp4'
    imageio.mimsave(str(output), [numpy.zeros((256, 256, 3), dtype=numpy.uint8)] * 3, fps=25)
    assert output.stat().st_size > 0
print(json.dumps({'ok': True, 'torch': torch.__version__, 'torchvision': torchvision.__version__, 'numpy': numpy.__version__, 'imageio': imageio.__version__, 'source': str(source), 'sys_path': sys.path}))
