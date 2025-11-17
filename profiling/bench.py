#-------=====================================================----------#
#                   
#                   Profiling and Benchmark Script.
#           
#-------=====================================================----------#

import os
import subprocess
import time
from enum import Enum
from argparse import ArgumentParser
from typing import List
from pathlib import Path
from typing import Optional

BENCHMARKS = ["jetstream", "speedometer", "top-k"]
PROFILES = ["ic-stubs"]
MODE_CONFIGS = {
    "DEFAULT": "",
    "GENERIC": "--no-jit-backend --no-baseline --no-ion",
    "BL_INTERP": "--no-ion --cache-ir-stubs=off",
    "BL_INTERP_NO_ICS": "--no-ion",
    "NO_JIT_BACKEND": "--no-jit-backend",
    "PBL": "TODO",
    "PBL_NO_ICS": "TODO"
}

class Runner:
    def __init__(self, ff_dir: Path, build_dir: Path):
        if not os.environ.get("MOZCONFIG"):
            print("Must supply MOZCONFIG environment variable.")
            exit(1)
        
        self.ff_dir = ff_dir
        self.ff_bin = build_dir / "dist/bin/firefox"
        self.js_bin = build_dir / "dist/bin/js"

    def run_shell_cmd(self, cmd, env):
        os.chdir(self.ff_dir)
        print(f"Running command from: {self.ff_dir.name}")
        start_t = time.time() 
        result = subprocess.run([cmd], shell=True, capture_output=True, env=env)
        end_t = time.time()
        print(f"Ran command in {round(end_t-start_t, 3)} with exit {result.returncode}")
    
    def profile(self, profiles, modes):
        for mode in modes:
            for profile in profiles:
                if profile == "ic-stubs":
                    log_dir = Path(input("Directory for stub logs (may be up to 500MB): ")).absolute()
                    if not log_dir.is_dir():
                        print(f"Supplied directory {log_dir} does not exist.")
                        continue 
                    
                    env = os.environ.copy()
                    env.update({
                        "JS_OPTIONS" : MODE_CONFIGS[mode],
                        "MOZ_DISABLE_CONTENT_SANDBOX": "1",
                        "IC_STAT_LOG_DIR": log_dir
                    })
                    
                    self.run_shell_cmd(cmd=str(self.ff_bin), env=env)
                   
    def benchmark(self, benchmarks, modes):
        for mode in modes:
            for benchmark in benchmarks:
                env = os.environ.copy()
                env["JS_OPTIONS"] = MODE_CONFIGS[mode]
                if benchmark == "speedometer":
                    print("Running Speedometer")
                    cmd = f"""./mach raptor --page-cycles 2 -t speedometer3
                               > ./speedometer_output_{mode}.log 2>&1"""
                elif benchmark == "jetstream":
                    print("Running JetStream")
                    cmd = f"./mach raptor --page-cycles 2 -t jetstream2 > ./jetstream_output_{mode}.log 2>&1"
                else:
                    print("TODO")
                    exit(1)
                print(f"Running command: {cmd}") 
                self.run_shell_cmd(cmd=cmd, env=env)
  
def validate_firefox_dir(dir_str) -> Path:
    ff_dir = Path(dir_str)
    if not ff_dir.exists() or not ff_dir.is_dir():
        print(f"Supplied invalid firefox directory: {args.firefox_dir}")
        exit(1)
    
    subdirs = [ file.name for file in ff_dir.iterdir() ]
    if not "mach" in subdirs:
        print(f"Could not find mach in firefox dir {args.firefox_dir}")
        exit(1)

    return ff_dir

def validate_build_dir(dir_str) -> Path:
    firefox_bin = Path(dir_str) / "dist/bin/firefox"
    if not firefox_bin.exists():
        print(f"Could not find firefox binary in: {str(firefox_bin)}")
        exit(1)
    return Path(dir_str)

if __name__ == "__main__": 
    parser = ArgumentParser() 

    parser.add_argument( "-f", "--firefox-dir", default=".",
        help="Benchmark(s) to run")
    
    parser.add_argument( "-b", "--bench", nargs="+", choices=BENCHMARKS,
        help="Benchmark(s) to run")
    
    parser.add_argument( "-p", "--profile", nargs="+", choices=PROFILES,
        help="Profile(s) to use")
    
    parser.add_argument( "-m", "--modes", nargs="+", choices=MODE_CONFIGS.keys(), default=["DEFAULT"],
        help="Mode(s) to run")

    parser.add_argument( "-d", "--build", required=True,
        help="Which build to run")
    
    args = parser.parse_args() 
    ff_dir = validate_firefox_dir(args.firefox_dir) 
    build_dir = validate_build_dir(args.build)
    runner = Runner(ff_dir, build_dir)
    if args.bench:
        assert(args.modes is not None)
        runner.benchmark(args.bench, args.modes)

    elif args.profile:
        assert(args.modes is not None and args.build is not None)
        runner.profile(args.profile, args.modes)

