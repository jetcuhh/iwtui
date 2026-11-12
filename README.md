# iwtui

Terminal UI for [iwd](https://iwd.wiki.kernel.org/), driving `iwctl`. No D-Bus.

    iwtui [-v | --version]

    make            # build
    make install    # ~/.local/bin (sudo: /usr/local/bin)

## Environment

| variable | meaning |
| --- | --- |
| `IWTUI_IWCTL` | path to `iwctl` |
| `IWTUI_CHECK_HOST` | connectivity-check `host[:port]` |
| `IWTUI_NO_CHECK=1` | disable the internet check |
| `IWTUI_PASS_ARGV=1` | pass the key on the command line (visible in `ps`) |
| `IWTUI_PRIV` | `sudo` or `doas` for QR passwords |
| `STATE_DIRECTORY` | iwd state dir (default `/var/lib/iwd`) |
