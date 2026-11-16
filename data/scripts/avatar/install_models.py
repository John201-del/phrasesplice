"""Fetch only the four required official 256/crop CPU inference artifacts."""
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from download_file import download, workspace_path

root = workspace_path(sys.argv[1])
manifest = json.loads(Path(__file__).with_name('model_artifacts.json').read_text(encoding='utf-8'))
for artifact in manifest:
    name = artifact['name']
    print(json.dumps({'downloading': name}), flush=True)
    download(artifact['url'], root / 'models' / name, artifact['bytes'], artifact['sha256'])
workspace_path(root / 'models-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'models': len(manifest), 'bytes': sum(item['bytes'] for item in manifest)}))
