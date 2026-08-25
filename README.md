# niri-edid-color

Experimental, panel-locked KMS colour correction for the built-in Chimei Innolux
N140HCA-EAC (`CMN 0x14D4`) display.

This helper is intended to reproduce the useful part of Plasma's **Built-in**
profile while keeping stock niri. It programs the Intel display engine's
pre-compositor colour stages:

```text
sRGB EOTF -> DEGAMMA_LUT -> EDID-derived CTM -> GAMMA_LUT supplied through niri
```

The helper deliberately owns only `DEGAMMA_LUT` and `CTM`. A persistent Wayland
gamma-control client must supply the final gamma 2.2 curve through niri. Niri
26.04 resets `GAMMA_LUT` when it creates the connector, but its current KMS code
does not manage `DEGAMMA_LUT` or `CTM`.

[`gammastep-regamma.ini`](gammastep-regamma.ini) contains the matching neutral
GammaStep configuration. It targets only `eDP-1`, holds gamma at 2.2, and does
not apply a day/night colour-temperature shift. GammaStep is installed from the
normal Arch/CachyOS repository.

## Current scope

- Exact 128-byte EDID match required.
- Exact connector `eDP-1` required.
- i915 atomic KMS with `DEGAMMA_LUT` and `CTM` required.
- `session-autostart` keeps both colour stages together for a niri session.
- The program is not a general ICC loader.

The embedded EDID SHA-256 is:

```text
cda4deb364442ee30054c0fb9455d85c50e10c50645eb1021704d1d4e96c55d1
```

## Build and non-mutating checks

```sh
make
make check
./niri-edid-color status
./niri-edid-color profile
```

`status`, `profile`, and `self-test` do not alter the display. `make check` only
runs offline checks and prints static information.

## Safety behaviour

`apply` and `reset`:

- refuse a different EDID;
- refuse unknown existing degamma or CTM state unless `--force` is supplied;
- use DRM's atomic `TEST_ONLY` validation before a real commit;
- support `--dry-run`, which stops after `TEST_ONLY`;
- do not modify mode, resolution, planes, framebuffer, HDR metadata, connector
  colourspace, BPC, or the compositor-owned gamma property;
- put both stages into hardware bypass on `reset`.

A real `apply` is refused while `GAMMA_LUT` is in bypass. Without the final
gamma 2.2 curve, enabling sRGB degamma would make the screen look very dark.
`--allow-bypass-gamma` exists only for a controlled pre-session arrangement in
which the gamma client is known to start immediately afterwards.

## Manual commands

```sh
# Validate the proposed state while temporarily taking DRM master; no commit.
pkexec ./niri-edid-color apply --takeover --dry-run

# In a separate terminal, keep the final curve active. Starting this before the
# KMS transform will temporarily make the picture too light.
gammastep -c ./gammastep-regamma.ini

# Apply after the persistent gamma 2.2 client is active.
pkexec ./niri-edid-color apply --takeover

# Remove only this helper's degamma and CTM state.
pkexec ./niri-edid-color reset --takeover
```

`--takeover` switches from the active VT to a free VT, obtains DRM master,
performs the requested operation, releases DRM master, and returns to the
original VT. Termination signals are blocked during the hand-off. It is still
experimental: a driver, compositor, or session-manager bug can leave you on the
temporary VT, in which case switch back with `Ctrl`+`Alt`+`F1`.

Rebooting clears these volatile KMS properties. The `reset` command is the
normal recovery path. `--force` should only be used when intentionally replacing
state created by another colour-management component.

`temporary-trial.sh` is an orchestration guard for visual testing. Given the PID
of an already-running GammaStep process, it applies the transform for 5–120
seconds and uses an `EXIT`/termination trap to reset KMS and stop that exact
GammaStep process afterwards.

## Automatic niri startup

The automatic arrangement has three layers:

- a root-owned copy of the compiled helper in `/usr/local/libexec`;
- fixed root-owned apply/reset wrappers authorized for the active local session
  by a Polkit rule;
- the unprivileged `session-autostart` supervisor launched only by niri.

The supervisor starts the neutral GammaStep curve, waits until `GAMMA_LUT` is
active, and then invokes the fixed apply action. On termination it resets the
EDID transform before stopping GammaStep. A runtime lock prevents duplicate
instances. No editable file in this project is authorized for passwordless root
execution.

Because stock niri owns DRM master and has no interface for `DEGAMMA_LUT` or
`CTM`, applying and resetting causes a short automatic VT switch. The screen may
flicker once shortly after niri starts.

### Installation ledger

This arrangement was installed and verified on 2026-08-25. The supervisor was
tested both ways: startup produced the expected three-stage pipeline, and clean
termination restored all three KMS properties to bypass.

The setup installs exactly these root-owned files:

```text
/usr/local/libexec/niri-edid-color
/usr/local/libexec/niri-edid-color-apply
/usr/local/libexec/niri-edid-color-reset
/etc/polkit-1/rules.d/49-niri-edid-color.rules
```

If `/usr/local/libexec` does not already exist, the setup creates it as
`root:root` with mode `0755`.

The first is a stable copy of the compiled binary. The other two wrappers ignore
all arguments and invoke only `apply --takeover` or `reset --takeover`. The
Polkit rule permits the apply wrapper only for an active local session. It also
permits the harmless reset wrapper after the session becomes inactive so logout
cleanup can always restore KMS bypass.

This rule is a deliberate persistent privilege-boundary change: any process in
the active local session can request the fixed apply operation without a
password, and a local session can request the fixed reset operation. It does not
authorize the editable project binary or arbitrary arguments/commands; both
authorized targets are root-owned wrappers that ignore caller arguments.

The setup adds exactly this line to
`~/.config/niri/cfg/autostart.kdl`:

```kdl
spawn-at-startup "/home/thomas/code/github.com/uhthomas/niri-edid-color/session-autostart"
```

It also installs the repository package `gammastep`. It does not enable the
packaged GammaStep systemd service, create a user service, or alter an ICC file.

While running, the supervisor creates these non-persistent files below
`$XDG_RUNTIME_DIR` (normally `/run/user/1000`):

```text
niri-edid-color.lock
niri-edid-color.pid
niri-edid-color.log
```

The PID file is removed on a clean exit. All three live only in the per-login
runtime directory and disappear on logout or reboot.

### Disable or re-enable the current session

To disable cleanly without logging out, ask the supervisor to terminate. Its
termination trap resets the KMS transform first and then stops GammaStep:

```sh
kill "$(cat "$XDG_RUNTIME_DIR/niri-edid-color.pid")"
```

To enable it again in the current niri session:

```sh
niri msg action spawn -- \
  /home/thomas/code/github.com/uhthomas/niri-edid-color/session-autostart
```

### Complete removal

If niri gains proper colour-management support, remove this workaround as
follows:

1. Disable the current session with the command above, or log out.
2. Remove the single `spawn-at-startup` line shown in the installation ledger.
3. Remove the four root-owned files:

   ```sh
   pkexec rm -f \
     /usr/local/libexec/niri-edid-color \
     /usr/local/libexec/niri-edid-color-apply \
     /usr/local/libexec/niri-edid-color-reset \
     /etc/polkit-1/rules.d/49-niri-edid-color.rules
   ```

   If this setup created `/usr/local/libexec` and it is empty afterwards, remove
   that directory too:

   ```sh
   pkexec rmdir /usr/local/libexec
   ```

4. If nothing else uses it, optionally uninstall GammaStep:

   ```sh
   pkexec pacman -Rns gammastep
   ```

5. Delete this project directory only if the source and documentation are no
   longer wanted.

After step 1, `./niri-edid-color status` should report `bypass` for
`DEGAMMA_LUT`, `CTM`, and `GAMMA_LUT`. A reboot also clears all volatile KMS
colour state.

## Embedded transform

The panel EDID reports gamma 2.20 and these chromaticities:

```text
R (0.5898, 0.3496)   G (0.3300, 0.5546)   B (0.1533, 0.1191)
White (0.3134, 0.3291)
```

The linear-light sRGB/D65 to panel RGB matrix is:

```text
 1.135772694  -0.253609549   0.109333397
-0.077937457   1.261426267  -0.181034903
-0.017785377  -0.035722018   1.056546260
```

Out-of-gamut sRGB colours are clipped by the display pipeline, as they are with
a relative-colorimetric compositor transform. This cannot expand the panel's
physical gamut; it corrects the mapping of colours that the panel can reproduce.
