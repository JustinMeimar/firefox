from argparse import ArgumentParser
from pathlib import Path
import shutil
import os

def separate_content_parent(dir: Path):
    assert dir.is_dir(), "Must be dir."
    
    parent_dir = dir / "parent"
    content_dir = dir / "content"
    parent_dir.mkdir(exist_ok=True)
    content_dir.mkdir(exist_ok=True)
    
    for file in dir.iterdir():
        if not file.is_file():
            continue
        if "parent" in file.name:
            shutil.move(file, parent_dir / file.name)
        elif "content" in file.name:
            shutil.move(file, content_dir / file.name)

if __name__ == "__main__":
    parser = ArgumentParser()
    parser.add_argument("dir")
    args = parser.parse_args()
        
    path = Path(args.dir)
    if not path.exists():
        print("Invalid Path") 
        exit(1)
    
    separate_content_parent(path)

