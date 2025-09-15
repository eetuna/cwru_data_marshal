
Devcontainer uses repository root as build context and Dockerfile at `docker/Dockerfile`.

If you see VS Code refer to `.devcontainer/docker/Dockerfile` or `.devcontainer/.devcontainer/Dockerfile`,
your settings or an extension is overriding the `build.context`/`build.dockerfile`. This repo's
`.devcontainer/devcontainer.json` sets:
- "context": ".."
- "dockerfile": "docker/Dockerfile"

Make sure your workspace root is the repository root and there is no `.code-workspace` file changing paths.
