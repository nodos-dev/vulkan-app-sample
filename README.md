# Sample Vulkan Process for Nodos

This folder contains a sample app for Nodos using Vulkan, with texture sharing & synchronization.

## Build Instructions
1. Download latest Nodos release from [nodos.dev](https://nodos.dev)
2. Clone the repository to the desired directory
```bash
git clone https://github.com/mediaz/nos-vulkan-app-sample.git --recurse-submodules
```
3. Generate project files using CMake:
```bash
cmake -S . -B Project -DNOSMAN_WORKSPACE_DIR=<path to Nodos workspace>
```
4. Build the project:
```bash
cmake --build Project
```

