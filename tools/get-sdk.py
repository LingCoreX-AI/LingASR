#!/usr/bin/env python3
"""MIT. Install the version-pinned release SDK components and assets (Python 3.12+)."""
import argparse,hashlib,pathlib,re,shutil,tarfile,tempfile,urllib.request
VERSION='0.1.0-preview.1'
REPOSITORY='LingCoreX-AI/LingASR'
NAME=f'lingasr-preview-sdk-{VERSION}'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive',type=pathlib.Path,help='offline SDK tar.gz; requires matching .sha256 file')
    parser.add_argument('--destination',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[1])
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='lingasr-sdk-') as temp:
        temp=pathlib.Path(temp)
        if args.archive:
            archive=args.archive.resolve();checksum=pathlib.Path(str(archive)+'.sha256').read_text()
        else:
            url=f'https://github.com/{REPOSITORY}/releases/download/v{VERSION}/{NAME}.tar.gz'
            archive=temp/(NAME+'.tar.gz')
            print(f'Downloading {REPOSITORY} v{VERSION}')
            with urllib.request.urlopen(url+'.sha256',timeout=30) as response:checksum=response.read(4096).decode()
            with urllib.request.urlopen(url,timeout=120) as response,archive.open('wb') as output:shutil.copyfileobj(response,output)
        parts=checksum.strip().split()
        if len(parts)!=2 or not re.fullmatch('[0-9a-f]{64}',parts[0]) or parts[1]!=NAME+'.tar.gz':
            raise ValueError('Invalid release checksum record')
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=parts[0]:raise ValueError('SDK archive checksum mismatch')
        unpack=temp/'unpacked';unpack.mkdir()
        with tarfile.open(archive) as tar:
            for item in tar.getmembers():
                target=(unpack/item.name).resolve()
                if not target.is_relative_to(unpack) or item.issym() or item.islnk() or not (item.isfile() or item.isdir()):
                    raise ValueError('Unsafe SDK archive member')
            tar.extractall(unpack,filter='data')
        sdk=unpack/NAME
        for line in (sdk/'SHA256SUMS').read_text().splitlines():
            digest,name=line.split('  ',1);path=(sdk/name).resolve()
            if not path.is_relative_to(sdk) or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
                raise ValueError('SDK file checksum mismatch')
        destination=args.destination.resolve();destination.mkdir(parents=True,exist_ok=True)
        for folder in ['components','assets','licenses']:
            shutil.copytree(sdk/folder,destination/folder,dirs_exist_ok=True)
        print(f'Installed LingASR Preview {VERSION} SDK components and assets in {destination}')
if __name__=='__main__':main()
