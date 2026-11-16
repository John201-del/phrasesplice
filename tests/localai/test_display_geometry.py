import importlib.util,json,hashlib,os,subprocess,sys,time,unittest
from pathlib import Path
import numpy as np

PROJECT=Path(__file__).resolve().parents[2]; DATA=Path(os.environ['LOCALCUT_WORKSPACE_ROOT']); QA=DATA/'tests/localai'
FF=PROJECT/'runtime/bin/ffmpeg.exe'; FP=FF.with_name('ffprobe.exe'); SCRIPT=PROJECT/'data/scripts/local_ai/process_media.py'; RUNTIME=DATA/'local-ai'
spec=importlib.util.spec_from_file_location('process_media',SCRIPT); module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)

def probe(path):
    return json.loads(subprocess.check_output([str(FP),'-v','error','-show_streams','-show_format','-of','json',str(path)]))

def video(metadata): return next(s for s in metadata['streams'] if s['codec_type']=='video')
def matrix(a,b,c,d): return f'\n00000000: {a} {b} 0\n00000001: {c} {d} 0\n00000002: 0 0 1073741824\n'

class DisplayGeometry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not all(path.is_file() for path in (FF, QA/'astronaut-nasa.png', QA/'speech-mix.wav', RUNTIME/'models/rvm_mobilenetv3_fp32.onnx')):
            raise unittest.SkipTest('Requires local AI models and media fixtures in the selected workspace')
        cls.source=QA/'rotation-nasa-90.mp4'; cls.sar=QA/'rotation-nonsquare.mp4'
        base=QA/'rotation-nasa-coded.mp4'
        subprocess.run([str(FF),'-v','error','-y','-loop','1','-i',str(QA/'astronaut-nasa.png'),'-i',str(QA/'speech-mix.wav'),'-t','1','-r','5','-vf','crop=384:512:64:0,scale=240:320,transpose=clock','-c:v','libx264','-threads','2','-pix_fmt','yuv420p','-c:a','aac',str(base)],check=True)
        subprocess.run([str(FF),'-v','error','-y','-display_rotation','90','-i',str(base),'-c','copy',str(cls.source)],check=True)
        subprocess.run([str(FF),'-v','error','-y','-i',str(base),'-vf','setsar=2/1','-c:v','libx264','-threads','2','-c:a','copy',str(cls.sar)],check=True)

    def call(self,operation,source,suffix):
        output=QA/('rotation-'+operation+suffix); output.unlink(missing_ok=True)
        command=[sys.executable,str(SCRIPT),'--operation',operation,'--input',str(source),'--output',str(output),'--ffmpeg',str(FF),'--ffprobe',str(FP),'--model-root',str(RUNTIME/'models'),'--runtime-root',str(RUNTIME)]
        start=time.monotonic(); result=subprocess.run(command,capture_output=True,text=True,timeout=60)
        events=[json.loads(line) for line in result.stdout.splitlines()]
        return result,events[-1],output,time.monotonic()-start

    def test_canonical_rotations_and_priority(self):
        for angle,values in [(0,(65536,0,0,65536)),(90,(0,-65536,65536,0)),(180,(-65536,0,0,-65536)),(270,(0,65536,-65536,0))]:
            with self.subTest(angle=angle):
                stream=dict(width=320,height=240,sample_aspect_ratio='1:1',tags={'rotate':'45'},side_data_list=[{'side_data_type':'Display Matrix','displaymatrix':matrix(*values),'rotation':angle}])
                self.assertEqual(module.video_geometry(stream),(240,320,angle) if angle%180 else (320,240,angle))
        for sar in [None,'N/A','0:1','1:1','2:2']:
            self.assertEqual(module.video_geometry(dict(width=320,height=240,sample_aspect_ratio=sar,tags={'rotate':'-90'})),(240,320,270))

    def test_unsupported_geometry_is_rejected(self):
        for values in [(65536,0,0,-65536),(32768,0,0,32768),(60000,10000,-10000,60000)]:
            with self.subTest(matrix=values):
                with self.assertRaises(ValueError): module.video_geometry(dict(width=320,height=240,side_data_list=[{'side_data_type':'Display Matrix','displaymatrix':matrix(*values),'rotation':0}]))
        for stream in [dict(width=320,height=240,sample_aspect_ratio='2:1'),dict(width=320,height=240,tags={'rotate':'45'}),dict(width=320,height=240,tags={'rotate':'nan'})]:
            with self.assertRaises(ValueError): module.video_geometry(stream)

    def test_rvm_upright_alpha_audio_source_integrity(self):
        original=hashlib.sha256(self.source.read_bytes()).hexdigest()
        result,final,output,seconds=self.call('remove_background',self.source,'.mov')
        self.assertEqual(result.returncode,0,(result.stderr,final)); self.assertTrue(final['ok'])
        metadata=probe(output); stream=video(metadata)
        self.assertEqual((stream['width'],stream['height']),(240,320))
        self.assertEqual(module.video_geometry(stream),(240,320,0))
        self.assertEqual(stream['avg_frame_rate'],'5/1'); self.assertEqual(stream['pix_fmt'],'argb')
        self.assertEqual(len([s for s in metadata['streams'] if s['codec_type']=='audio']),1)
        pcm=[]
        for path in (self.source,output):
            pcm.append(subprocess.check_output([str(FF),'-v','error','-i',str(path),'-map','0:a:0','-c:a','pcm_s16le','-f','s16le','pipe:1']))
        self.assertEqual(pcm[0],pcm[1])
        self.assertAlmostEqual(float(metadata['format']['duration']),float(probe(self.source)['format']['duration']),delta=.05)
        raw=subprocess.check_output([str(FF),'-v','error','-i',str(output),'-vf','alphaextract','-f','rawvideo','-pix_fmt','gray','pipe:1'])
        alpha=np.frombuffer(raw,np.uint8).reshape(-1,320,240)
        self.assertGreater(float(alpha[:,30:95,70:125].mean()),220); self.assertGreater(float(np.mean(alpha<20)),.1)
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).hexdigest(),original)
        evidence=dict(seconds=seconds,source_sha256=original,source_unchanged=True,audio_pcm_bit_exact=True,audio_pcm_sha256=hashlib.sha256(pcm[0]).hexdigest(),source=probe(self.source),output=metadata,face_alpha_mean=float(alpha[:,30:95,70:125].mean()),transparent_fraction=float(np.mean(alpha<20)))
        (QA/'rotation-evidence.json').write_text(json.dumps(evidence,indent=2),encoding='utf8')
        subprocess.run([str(FF),'-v','error','-y','-i',str(output),'-frames:v','1',str(QA/'rotation-nasa-matte.png')],check=True)

    def test_haar_sees_upright_face(self):
        result,final,output,_=self.call('smooth_skin',self.source,'.mp4')
        self.assertEqual(result.returncode,0,(result.stderr,final)); self.assertTrue(final['ok'])
        self.assertEqual(module.video_geometry(video(probe(output))),(240,320,0))

    def test_non_square_video_rejected_but_audio_allowed(self):
        result,final,output,_=self.call('enhance_video',self.sar,'.mp4')
        self.assertNotEqual(result.returncode,0); self.assertFalse(final['ok']); self.assertFalse(output.exists()); self.assertIn('aspect',final['message'])
        result,final,output,_=self.call('denoise_audio',self.sar,'.wav')
        self.assertEqual(result.returncode,0,final); self.assertTrue(final['ok'])

    def test_enhancement_normalizes_rotation(self):
        result,final,output,_=self.call('enhance_video',self.source,'.mp4')
        self.assertEqual(result.returncode,0,final)
        self.assertEqual(module.video_geometry(video(probe(output))),(240,320,0))

if __name__=='__main__': unittest.main()
