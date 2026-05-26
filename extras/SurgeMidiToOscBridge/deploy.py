"""
deploy.py
Deploys SurgeMidiToOscBridge binary and Whisper models to an RPi5 target via SFTP.

    python deploy.py --target-ip 192.168.1.100
    python deploy.py --target-ip 192.168.1.100 --deploy-path ~/bridge

NOTE: stop any running SurgeMidiToOscBridge processes on the target before
deploying — SFTP cannot overwrite a binary that is currently executing.

After deploy, on the Pi:
    ~/bridge/SurgeMidiToOscBridge [--model tiny|base|small]
"""

import os
import stat
import getpass
import argparse

try:
    import paramiko
except ImportError:
    raise SystemExit("pip install paramiko")

_DIST = os.path.join(os.path.dirname(__file__), "build", "dist")

def main():
    parser = argparse.ArgumentParser(description="Deploy SurgeMidiToOscBridge to RPi5 via SFTP")
    parser.add_argument("--target-ip",   required=True,
                        help="Target IP or hostname (e.g. 192.168.1.100 or user@192.168.1.100)")
    parser.add_argument("--deploy-path", default="~/bridge",
                        help="Destination directory on target (default: ~/bridge)")
    args = parser.parse_args()

    binary = os.path.join(_DIST, "SurgeMidiToOscBridge")
    if not os.path.exists(binary):
        raise SystemExit(f"Binary not found: {binary}\nRun  ./build.sh  first.")

    user, _, host = args.target_ip.rpartition('@')
    password = getpass.getpass(f"Password for {args.target_ip}: ")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(host, username=user or None, password=password)

    deploy_dir = args.deploy_path
    if '~' in deploy_dir:
        _, stdout, _ = client.exec_command('echo $HOME')
        home = stdout.read().decode().strip()
        deploy_dir = deploy_dir.replace('~', home)

    client.exec_command(f'mkdir -p "{deploy_dir}/models"')

    sftp = client.open_sftp()

    def sftp_put_if_changed(local, remote, label):
        local_size = os.path.getsize(local)
        try:
            remote_size = sftp.stat(remote).st_size
            if remote_size == local_size:
                print(f"  ✓ {label} (unchanged)")
                return
        except FileNotFoundError:
            pass
        print(f"  → {label}")
        sftp.put(local, remote)

    remote_bin = deploy_dir.rstrip('/') + '/SurgeMidiToOscBridge'
    sftp_put_if_changed(binary, remote_bin, "SurgeMidiToOscBridge")
    sftp.chmod(remote_bin, 0o755)

    models_src = os.path.join(_DIST, "models")
    if os.path.isdir(models_src):
        for dirpath, _, filenames in os.walk(models_src):
            rel_dir = os.path.relpath(dirpath, _DIST).replace(os.sep, '/')
            remote_dir = deploy_dir.rstrip('/') + '/' + rel_dir
            client.exec_command(f'mkdir -p "{remote_dir}"')
            for fname in sorted(filenames):
                local  = os.path.join(dirpath, fname)
                remote = remote_dir + '/' + fname
                sftp_put_if_changed(local, remote, f"{rel_dir}/{fname}")

    sftp.close()
    client.close()

    print(f"\nDeployed to {args.target_ip}:{deploy_dir}")
    print(f"\nTo run on the Pi:")
    print(f"  {deploy_dir}/SurgeMidiToOscBridge [--model tiny|base|small]")

if __name__ == "__main__":
    main()
