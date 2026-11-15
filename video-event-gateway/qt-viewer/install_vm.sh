#!/usr/bin/env bash
set -euo pipefail

source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-${source_dir}/build}"
prefix="${HOME}/.local"

cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}" -j2
cmake --install "${build_dir}" --prefix "${prefix}"

desktop_dir="${prefix}/share/applications"
mkdir -p -- "${desktop_dir}"
cat > "${desktop_dir}/edge-av-qt-viewer.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Edge AV 直播播放器
Comment=播放审核网关的 RTSP 放行流
Exec=/usr/bin/env QT_QPA_PLATFORM=xcb ${prefix}/bin/veg_qt_viewer
TryExec=${prefix}/bin/veg_qt_viewer
Icon=video-display
Categories=AudioVideo;Video;
Terminal=false
StartupNotify=true
EOF
printf 'Installed viewer and desktop launcher under %s\n' "${prefix}"
