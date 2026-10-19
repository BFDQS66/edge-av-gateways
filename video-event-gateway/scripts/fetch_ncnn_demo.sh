#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
download_dir="${project_root}/.cache/yolov8-ncnn-example"
mkdir -p "${project_root}/.cache" "${project_root}/data/models"
if [[ ! -d "${download_dir}/.git" ]]; then
  git clone https://github.com/Qengineering/YoloV8-ncnn-Raspberry-Pi-4.git "${download_dir}"
fi
git -C "${download_dir}" checkout 3342965252320bb4aca1835efbd115bef4653034
cp "${download_dir}/yolov8n.param" "${project_root}/data/models/yolov8n.param"
cp "${download_dir}/yolov8n.bin" "${project_root}/data/models/yolov8n.bin"
cp "${download_dir}/busstop.jpg" "${project_root}/data/people_demo.jpg"
echo "Demo model and image copied into ${project_root}/data"
