"""Download a fixed public artifact in bounded ranges; verify size and optional SHA."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_avatar import workspace_path

def download(url, destination, size, sha=None):
    destination = workspace_path(destination)
    if destination.is_file() and destination.stat().st_size == size:
        with destination.open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if not sha or actual == sha:
            return actual
    parts = workspace_path(destination.with_name(destination.name + '.parts'))
    parts.mkdir(exist_ok=True)
    chunk = 4 * 1024 * 1024
    def fetch(start):
        end = min(start + chunk, size) - 1
        part = workspace_path(parts / str(start))
        if not part.exists() or part.stat().st_size != end - start + 1:
            range_url = url + ('&' if '?' in url else '?') + f'avatar_range={start}'
            result = subprocess.run(['curl.exe', '-sS', '-L', '--fail', '--retry', '4', '--retry-all-errors', '--connect-timeout', '20', '--speed-time', '30', '--speed-limit', '1024', '--max-time', '120', '-r', f'{start}-{end}', '-o', str(part), range_url], capture_output=True)
            if result.returncode or part.stat().st_size != end - start + 1:
                raise RuntimeError('artifact range download failed: ' + destination.name)
        return part
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        filenames = list(pool.map(fetch, range(0, size, chunk)))
    temporary = workspace_path(destination.with_name(destination.name + '.partial'))
    with temporary.open('wb') as output:
        for part in filenames:
            with part.open('rb') as stream:
                while block := stream.read(1024 * 1024):
                    output.write(block)
    with temporary.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if temporary.stat().st_size != size or sha and sha != actual:
        temporary.unlink()
        for part in filenames:
            part.unlink()
        parts.rmdir()
        raise RuntimeError('artifact checksum mismatch: ' + destination.name)
    temporary.replace(destination)
    for part in filenames:
        part.unlink()
    parts.rmdir()
    return actual

if __name__ == '__main__':
    print(json.dumps({'sha256': download(sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else None)}))
