#!/bin/bash
set -e

if [ -z "$1" ]; then
  echo "Usage: ./lib_release.sh 1.2.4"
  exit 1
fi

VERSION="$1"
TAG="v$VERSION"
BRANCH="$(git branch --show-current)"

if [ "$BRANCH" != "main" ]; then
  echo "ERROR: Please run on main branch (current: $BRANCH)"
  exit 1
fi

# 1) library.properties version bump
if [ ! -f "library.properties" ]; then
  echo "ERROR: library.properties not found"
  exit 1
fi

# macOS sed 대응
if [[ "$OSTYPE" == "darwin"* ]]; then
  sed -i '' "s/^version=.*/version=$VERSION/" library.properties
else
  sed -i "s/^version=.*/version=$VERSION/" library.properties
fi

# 2) sanity check: version line exists
grep -q "^version=$VERSION$" library.properties || (echo "ERROR: version update failed" && exit 1)

# 3) commit
git add library.properties
git commit -m "chore: bump library version to $VERSION"

# 4) tag (중복 방지)
if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "ERROR: tag $TAG already exists"
  exit 1
fi

git tag -a "$TAG" -m "Release $TAG"

# 5) push
git push origin main
git push origin "$TAG"

echo "✅ Released $TAG (library.properties version=$VERSION)"

