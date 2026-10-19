# Repository workflow

- This repository publishes `speech-translation-gateway/` and `video-event-gateway/` for portfolio review. Keep unrelated Zynq and PetaLinux workspace files outside Git.
- After each requested update, run relevant checks, commit the resulting source or documentation changes, and push the commit to `origin/main` when GitHub is available.
- Never commit credentials, model weights, recordings, generated logs, build outputs, or local dependency caches. Review the staged file list before committing.
- Keep each project's README and deployment notes aligned with actual behavior and measured results.
