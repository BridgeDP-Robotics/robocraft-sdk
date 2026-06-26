#!/bin/bash
# 本地预览 GitHub Pages（Jekyll）
# 用法: ./scripts/preview_docs.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# 安装到项目 vendor/bundle，避免写入 /var/lib/gems（需 root）
bundle config set --local path 'vendor/bundle'
bundle install

echo ""
echo "=== 文档预览 ==="
echo "浏览器打开: http://127.0.0.1:4000/robocraft-sdk/"
echo "按 Ctrl+C 停止"
echo ""

bundle exec jekyll serve --source docs --livereload
