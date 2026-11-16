import importlib.util,json,numpy as np,wave,subprocess,sys
from pathlib import Path
spec=importlib.util.spec_from_file_location('verify',Path(__file__).with_name('verify_models.py')); v=importlib.util.module_from_spec(spec); spec.loader.exec_module(v)
q=v.QA; clean=v.audio(q/'speech-clean.wav'); mix=np.clip(clean+np.random.default_rng(7).normal(0,.035,len(clean)),-1,1)
source=q/'speech-white-noise.wav'; output=q/'white-denoised.wav'; output.unlink(missing_ok=True)
with wave.open(str(source),'wb') as h:
    h.setparams((1,2,44100,0,'NONE','none')); h.writeframes((mix*32767).astype('<i2').tobytes())
cmd=[sys.executable,str(v.PROJECT/'data/scripts/local_ai/process_media.py'),'--operation','denoise_audio','--input',str(source),'--output',str(output),'--ffmpeg',str(v.FFMPEG),'--ffprobe',str(v.FFPROBE),'--model-root',str(v.RUNTIME/'models'),'--runtime-root',str(v.RUNTIME)]
subprocess.run(cmd,check=True)
metrics={'mix_sisdr_db':v.sisdr(clean,v.audio(source)),'output_sisdr_db':v.sisdr(clean,v.audio(output))}
(q/'white-noise-evidence.json').write_text(json.dumps(metrics,indent=2),encoding='utf8'); print(json.dumps(metrics))
