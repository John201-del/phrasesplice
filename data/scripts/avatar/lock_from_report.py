"""Audit the isolated resolver output and freeze every selected distribution."""
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_avatar import workspace_path

report = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
local_wheels = workspace_path(Path(sys.argv[1]).parent).as_uri() + '/torch'
lines = []
for item in report['install']:
    name = item['metadata']['name']
    normalized = name.lower().replace('_', '-')
    if any(value in normalized for value in ('nvidia', 'cuda', 'tensorflow', 'gradio', 'gfpgan', 'basicsr', 'realesrgan')):
        raise RuntimeError('Unexpected heavyweight dependency: ' + name)
    version = item['metadata']['version']
    download = item['download_info']
    url = download['url']
    if not url.startswith(('https://files.pythonhosted.org/', local_wheels)):
        raise RuntimeError('Unexpected package host: ' + name)
    sha = download['archive_info']['hashes']['sha256']
    lines.append(f'{name}=={version} --hash=sha256:{sha}')
workspace_path(sys.argv[2]).write_text('\n'.join(sorted(lines, key=str.lower)) + '\n', encoding='utf-8')
print(json.dumps({'packages': len(lines), 'heavyweight_dependencies': False}))
