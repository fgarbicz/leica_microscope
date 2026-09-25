"""Renders docs/USER_GUIDE.md to a standalone HTML page (for the Start menu shortcut)."""
import sys, pathlib, markdown

src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
body = markdown.markdown(src.read_text(encoding='utf-8'), extensions=['tables', 'fenced_code', 'toc'])
css = """
body { font-family: 'Segoe UI', Arial, sans-serif; max-width: 860px; margin: 32px auto; padding: 0 20px;
       line-height: 1.55; color: #1e1e1e; }
h1 { color: #1b4f8a; border-bottom: 2px solid #1b4f8a; padding-bottom: 6px; }
h2 { color: #1b4f8a; margin-top: 32px; }
h3 { color: #264d73; }
table { border-collapse: collapse; margin: 12px 0; }
th, td { border: 1px solid #c8d2dc; padding: 5px 10px; text-align: left; vertical-align: top; }
th { background: #e8eef5; }
code { background: #f1f3f5; padding: 1px 4px; border-radius: 3px; }
"""
dst.write_text(f"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'><title>DM Imaging user guide</title>"
               f"<style>{css}</style></head><body>{body}</body></html>", encoding='utf-8')
print(f'wrote {dst}')
