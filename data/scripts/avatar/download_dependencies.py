"""Download the audited wheel set from immutable official distribution URLs."""
import concurrent.futures
import json
from pathlib import Path
import re
import subprocess
import sys
import hashlib
from urllib.parse import unquote, urlparse
sys.path.insert(0, str(Path(__file__).resolve().parent))
from download_file import download, workspace_path

root = workspace_path(sys.argv[1])
wheels = workspace_path(root / 'wheels')
wheels.mkdir(exist_ok=True)
manifest_path = Path(__file__).with_name('dependency_artifacts.json')
if manifest_path.exists():
    artifacts = json.loads(manifest_path.read_text(encoding='utf-8'))
else:
    report = json.loads((root / 'dependencies-resolve.json').read_text(encoding='utf-8'))
    artifacts = []
    for item in report['install']:
        info = item['download_info']
        if info['url'].startswith('file:'):
            continue
        artifacts.append({'url': info['url'], 'sha256': info['archive_info']['hashes']['sha256'], 'name': unquote(Path(urlparse(info['url']).path).name)})

def fetch(artifact):
    existing = workspace_path(wheels / artifact['name'])
    if existing.is_file():
        with existing.open('rb') as stream:
            valid = hashlib.file_digest(stream, 'sha256').hexdigest() == artifact['sha256']
        if valid:
            artifact['bytes'] = existing.stat().st_size
            return artifact
    if 'bytes' not in artifact:
        headers = subprocess.run(['curl.exe', '-sSI', '--fail', '--retry', '4', '--retry-all-errors', '--connect-timeout', '20', '--max-time', '60', artifact['url']], capture_output=True, text=True, check=True).stdout
        sizes = re.findall(r'(?im)^Content-Length:\s*(\d+)', headers)
        if not sizes:
            raise RuntimeError('Distribution size unavailable: ' + artifact['name'])
        artifact['bytes'] = int(sizes[-1])
    download(artifact['url'], wheels / artifact['name'], artifact['bytes'], artifact['sha256'])
    print(json.dumps({'downloaded': artifact['name']}), flush=True)
    return artifact

with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    artifacts = list(pool.map(fetch, artifacts))
workspace_path(root / 'dependency-artifacts.json').write_text(json.dumps(artifacts, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'verified_artifacts': len(artifacts), 'bytes': sum(item['bytes'] for item in artifacts)}))
