"""Reject board-specific secrets, local paths and Persian text in public source."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
TEXT_SUFFIXES = {".md", ".txt", ".py", ".ps1", ".cmd", ".tcl", ".c", ".h",
                 ".s", ".ld", ".cfg", ".json", ".yml", ".yaml", ".html",
                 ".css", ".js", ".gitignore"}
EXCLUDED = {".git", ".cache", ".local-test", "dist", "runtime", "build", "build-shadow", "__pycache__"}
PROHIBITED = re.compile("|".join((
    "003A0045" + "3332511834353830",
    "0039001D" + "3332511834353830",
    "G100-b02" + "-test",
    r"C:[\\/]Files[\\/]" + "Programming",
)), re.IGNORECASE)


def main() -> None:
    problems = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or set(path.relative_to(ROOT).parts) & EXCLUDED:
            continue
        if path.suffix.lower() not in TEXT_SUFFIXES and path.name != ".gitignore":
            continue
        try:
            value = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError:
            continue
        if any(0x0600 <= ord(char) <= 0x06FF or 0x0750 <= ord(char) <= 0x077F
               for char in value):
            problems.append(f"Persian/Arabic script: {path.relative_to(ROOT)}")
        if PROHIBITED.search(value):
            problems.append(f"Board-specific data or secret: {path.relative_to(ROOT)}")
    if problems:
        raise SystemExit("\n".join(problems))
    print("Public source scan passed")


if __name__ == "__main__":
    main()
