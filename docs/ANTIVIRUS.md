# Antivirus detections

GrimAction loads a runtime DLL into Grim Dawn and installs hooks for camera control. Antivirus software can flag this behavior. A detection should be investigated rather than automatically dismissed.

## Verify the download

- Download the package from the GrimAction GitHub Releases page.
- Compare the ZIP's SHA-256 with the published checksum.
- The launcher checks fixed package files against `bin/package-manifest.json` before starting.
- Source and build instructions are available in the repository. Hash checks establish file integrity; they do not independently establish safety.

If a file is quarantined, stop and review the detection. You can submit the affected file to your antivirus vendor for analysis. Do not disable system-wide protection to run GrimAction.

GrimAction is unsigned. No game files are replaced by the player package, and session logs remain on your computer.
