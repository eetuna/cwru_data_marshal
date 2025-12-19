
Devcontainer builds from `.devcontainer/Dockerfile` (context is `.devcontainer/`) and mounts the repo into `/workspaces/cwru_data_marshal`.

If you see VS Code refer to paths outside `.devcontainer/`, your settings or an extension is overriding the `build.context`/`build.dockerfile`. This repo's `.devcontainer/devcontainer.json` sets:
- "context": "."
- "dockerfile": "Dockerfile"

Make sure your workspace root is the repository root and there is no `.code-workspace` file changing paths.
