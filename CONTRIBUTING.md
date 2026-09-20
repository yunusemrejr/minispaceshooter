# Contributing

This GitHub repository is the canonical shared history for Mini Space Shooter.
Future local changes should be committed and pushed here as they are completed,
so the repository stays synchronized with the working game.

## Local workflow

1. Start from the latest `main` branch:

   ```bash
   git pull --rebase origin main
   ```

2. Make and verify the change. At minimum, run:

   ```bash
   ./run.sh test
   ```

   Use `./run.sh strict`, `./run.sh sanitize`, `./run.sh audio-test`, or
   `./run.sh window-test` when the change touches those areas.

3. Commit the source and documentation together with a concise message:

   ```bash
   git add <changed-files>
   git commit -m "type(scope): describe the change"
   ```

4. Push the completed local change back to GitHub:

   ```bash
   git push origin main
   ```

Do not commit generated `build/` or `out/` files, local agent/session metadata,
diagnostic dumps, credentials, or personal configuration. Update `README.md`
when controls, requirements, behavior, or verification instructions change.

