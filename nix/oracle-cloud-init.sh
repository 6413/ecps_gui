#!/bin/bash
# Oracle Cloud cloud-init: sets up the ecps server on a new instance.
# Paste this whole file into "Advanced options > Initialization script > Paste cloud-init script".
#
# What it does: installs Nix, builds the server from github:6413/ecps_gui (x86_64 only, the flake has no ARM
# build), runs it as a systemd service on port 43254 (TCP and UDP), and opens that port in the instance firewall.
# The VCN security list is separate: add ingress rules for TCP 43254 and UDP 43254 from 0.0.0.0/0 in the console.
#
# Progress: sudo tail -f /var/log/ecps-setup.log     Server log: journalctl -u ecps-server -f
# Update to the newest commit later: sudo ecps-server-update
set -euo pipefail
exec > >(tee -a /var/log/ecps-setup.log) 2>&1
echo "=== ecps server setup $(date)"

PORT=43254
FLAKE=github:6413/ecps_gui

if [ "$(uname -m)" != "x86_64" ]; then
  echo "This instance is $(uname -m). The ecps flake builds for x86_64 only: pick an AMD or Intel shape."
  exit 1
fi

# ---- swap: the build compiles C++ modules with clang, 1 GB of memory (the free micro shape) is not enough
if [ "$(awk '/MemTotal/ {print $2}' /proc/meminfo)" -lt 6000000 ] && ! swapon --show | grep -q /swapfile; then
  fallocate -l 6G /swapfile || dd if=/dev/zero of=/swapfile bs=1M count=6144
  chmod 600 /swapfile
  mkswap /swapfile
  swapon /swapfile
  echo '/swapfile none swap sw 0 0' >> /etc/fstab
fi

# ---- tools the installer needs (Oracle Linux uses dnf, Ubuntu apt). They are usually there already, and right
# after the first boot the package mirrors can be out of reach, so a failure here is not the end
if command -v dnf >/dev/null; then
  dnf install -y curl xz tar git || echo "package install failed, going on with what is installed"
elif command -v apt-get >/dev/null; then
  { apt-get update -y && apt-get install -y curl xz-utils tar git; } || echo "package install failed, going on with what is installed"
fi

# ---- wait for the internet: at first boot the network can come up after this script starts
for i in $(seq 1 60); do
  curl -sfI https://github.com >/dev/null && break
  echo "waiting for the internet ($i)"
  sleep 5
done

# ---- Nix, with flakes on (the Determinate installer also handles SELinux on Oracle Linux)
if [ ! -x /nix/var/nix/profiles/default/bin/nix ]; then
  curl --proto '=https' --tlsv1.2 -sSf -L https://install.determinate.systems/nix | sh -s -- install --no-confirm
fi
NIX=/nix/var/nix/profiles/default/bin/nix

# ---- the update command: builds the newest server and restarts it
cat > /usr/local/bin/ecps-server-update <<EOF
#!/bin/bash
set -euo pipefail
echo "building the ecps server from $FLAKE (the first build takes a while)"
# the repo has no flake.lock, nix makes one for the build and must not try to write it back to GitHub
$NIX build --refresh --no-write-lock-file "$FLAKE#ecps-server" --out-link /opt/ecps/server
# SELinux (Oracle Linux) only lets systemd start programs labelled as programs
if command -v getenforce >/dev/null && [ "\$(getenforce)" = "Enforcing" ]; then
  chcon -t bin_t "\$(readlink -f /opt/ecps/server/bin/ecps-server)" || true
fi
systemctl restart ecps-server 2>/dev/null || true
echo "done: \$(readlink -f /opt/ecps/server)"
EOF
chmod +x /usr/local/bin/ecps-server-update
# sudo on Oracle Linux does not look in /usr/local/bin, a link where it does look
ln -sf /usr/local/bin/ecps-server-update /usr/bin/ecps-server-update

# ---- a user for the server, and its folder (ecps_server.key, the server's identity, is kept there)
id ecps >/dev/null 2>&1 || useradd --system --home-dir /var/lib/ecps --create-home --shell /usr/sbin/nologin ecps
mkdir -p /opt/ecps /var/lib/ecps
chown ecps:ecps /var/lib/ecps

cat > /etc/systemd/system/ecps-server.service <<EOF
[Unit]
Description=ecps server
After=network-online.target
Wants=network-online.target

[Service]
User=ecps
WorkingDirectory=/var/lib/ecps
ExecStart=/opt/ecps/server/bin/ecps-server $PORT
StandardInput=null
Restart=always
RestartSec=3
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload

/usr/local/bin/ecps-server-update
systemctl enable --now ecps-server

# ---- the instance's own firewall (Oracle Linux: firewalld, Ubuntu images: iptables rules that reject the rest)
if command -v firewall-cmd >/dev/null && systemctl is-active --quiet firewalld; then
  firewall-cmd --permanent --add-port=$PORT/tcp
  firewall-cmd --permanent --add-port=$PORT/udp
  firewall-cmd --reload
elif command -v iptables >/dev/null; then
  for proto in tcp udp; do
    iptables -C INPUT -p $proto --dport $PORT -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p $proto --dport $PORT -j ACCEPT
  done
  if command -v netfilter-persistent >/dev/null; then
    netfilter-persistent save
  elif [ -d /etc/iptables ]; then
    iptables-save > /etc/iptables/rules.v4
  fi
fi

echo "=== ecps server is running on port $PORT"
systemctl --no-pager status ecps-server || true
echo "Remember the VCN security list: ingress TCP $PORT and UDP $PORT from 0.0.0.0/0"
