"""Private CPU child; inherits the caller's Windows Job membership."""
from pathlib import Path
import runpy
import socket
import sys
import random
import os
sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_avatar import workspace_path

source, models, job = map(workspace_path, sys.argv[1:])
for key, relative in (('MPLCONFIGDIR', 'cache/matplotlib'), ('JOBLIB_TEMP_FOLDER', 'cache/tmp'), ('NUMBA_CACHE_DIR', 'cache/numba')):
    directory = workspace_path(models.parent / relative)
    directory.mkdir(parents=True, exist_ok=True)
    os.environ[key] = str(directory)
os.environ['MPLBACKEND'] = 'Agg'
os.environ['TORCH_FORCE_WEIGHTS_ONLY_LOAD'] = '1'
sys.path.insert(0, str(source))
import torch
import numpy as np
torch.set_num_threads(4)
torch.set_num_interop_threads(1)
torch.manual_seed(0)
np.random.seed(0)
random.seed(0)

def offline(*args, **kwargs):
    raise RuntimeError('network is disabled during avatar inference')

socket.socket.connect = offline
socket.create_connection = offline
sys.argv = [str(source / 'inference.py'), '--cpu', '--size', '256', '--preprocess', 'crop', '--batch_size', '1', '--source_image', str(job / 'portrait.png'), '--driven_audio', str(job / 'speech.wav'), '--checkpoint_dir', str(models), '--result_dir', str(job / 'results')]
try:
    runpy.run_path(str(source / 'inference.py'), run_name='__main__')
except ValueError as error:
    if str(error) == 'AVATAR_NO_FACE':
        sys.exit(20)
    raise
