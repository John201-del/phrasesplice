import importlib.util,json,numpy as np
from pathlib import Path
spec=importlib.util.spec_from_file_location('verify',Path(__file__).with_name('verify_models.py')); v=importlib.util.module_from_spec(spec); spec.loader.exec_module(v)
q=v.QA; clean=v.audio(q/'speech-clean.wav'); mix=v.audio(q/'speech-mix.wav'); vocals=v.audio(q/'verify-separate_vocals.wav'); background=v.audio(q/'verify-accompaniment.wav'); count=min(len(mix),len(vocals),len(background)); total=vocals[:count]+background[:count]
metrics={'samples':count,'reconstruction_relative_l2':float(np.linalg.norm(total-mix[:count])/np.linalg.norm(mix[:count])),'vocals_accompaniment_correlation':float(np.corrcoef(vocals[:count],background[:count])[0,1]),'vocals_sisdr_db':v.sisdr(clean,vocals),'input_sisdr_db':v.sisdr(clean,mix),'vocals_rms':float(np.sqrt(np.mean(vocals**2))),'background_rms':float(np.sqrt(np.mean(background**2)))}
(q/'separation-quality.json').write_text(json.dumps(metrics,indent=2),encoding='utf8'); print(json.dumps(metrics))
