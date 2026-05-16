#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
collect_ipaddr.py
Collect MT798* defconfig files whose CONFIG_IPADDR value differs from
the common default "192.168.1.1" and export the summary as Markdown.

Output is organized by platform (mt7981/mt7986/mt7987/mt7988) and lists
the defconfig filename, model name, and detected IP address value.

Usage:
	python collect_ipaddr.py [config_dir] [output_filename]

Note: Supports both regular files and symbolic links.
"""
import datetime
import os
import re
import sys


DEFAULT_IPADDR = "192.168.1.1"
PLATFORMS = ["mt7981", "mt7986", "mt7987", "mt7988"]


def parse_defconfig(filepath):
	"""Parse a defconfig file, following symlinks if necessary."""
	actual_filepath = os.path.realpath(filepath)

	try:
		with open(actual_filepath, "r", encoding="utf-8", errors="ignore") as f:
			content = f.read()
	except Exception:
		with open(actual_filepath, "rb") as f:
			content = f.read().decode("utf-8", errors="ignore")

	filename = os.path.basename(filepath)
	name = filename[:-len("_defconfig")] if filename.endswith("_defconfig") else filename
	parts = name.split("_", 1)
	platform = parts[0].lower()
	model = parts[1] if len(parts) > 1 else ""

	ipaddr = None
	use_ipaddr = False
	net_force_ipaddr = False

	m = re.search(r'^CONFIG_IPADDR="([^"]+)"$', content, re.MULTILINE)
	if m:
		ipaddr = m.group(1)

	if "CONFIG_USE_IPADDR=y" in content:
		use_ipaddr = True
	if "CONFIG_NET_FORCE_IPADDR=y" in content:
		net_force_ipaddr = True

	if not ipaddr:
		return None

	return {
		"filename": filename,
		"platform": platform,
		"model": model,
		"ipaddr": ipaddr,
		"is_default": ipaddr == DEFAULT_IPADDR,
		"use_ipaddr": use_ipaddr,
		"net_force_ipaddr": net_force_ipaddr,
	}


def collect(dirpath):
	"""Collect all mt798* defconfig files with CONFIG_IPADDR."""
	results = []
	try:
		names = sorted(os.listdir(dirpath))
	except Exception as e:
		print(f"Error accessing directory {dirpath}: {e}", file=sys.stderr)
		return results

	for fn in names:
		if not fn.endswith("_defconfig"):
			continue
		if not fn.startswith(tuple(PLATFORMS)):
			continue

		fp = os.path.join(dirpath, fn)
		if os.path.isfile(fp) or os.path.islink(fp):
			try:
				entry = parse_defconfig(fp)
				if entry:
					results.append(entry)
			except Exception as e:
				print(f"Error parsing file {fn}: {e}", file=sys.stderr)

	return results


def render_md(entries, outpath):
	"""Render entries as Markdown table."""
	now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
	lines = []
	lines.append("# MT798 IP Address Summary\n\n")
	lines.append(f"Generated: {now}\n\n")
	lines.append(
		f"This document lists mt798* defconfigs whose `CONFIG_IPADDR` value is not `{DEFAULT_IPADDR}`.\n\n"
	)

	for platform in PLATFORMS:
		lines.append(f"## {platform.upper()} Platform\n\n")
		filtered = [e for e in entries if e["platform"] == platform]
		if not filtered:
			lines.append("No data.\n\n")
			continue

		non_default = [e for e in filtered if not e["is_default"]]
		default_count = len(filtered) - len(non_default)

		if not non_default:
			lines.append(
				f"All {len(filtered)} configuration files use the default `CONFIG_IPADDR={DEFAULT_IPADDR}`.\n\n"
			)
			lines.append("| Filename | Model | IPADDR | USE_IPADDR | NET_FORCE_IPADDR |\n")
			lines.append("|---|---|---|---|---|\n")
			for e in filtered:
				fname = f'`{e["filename"]}`'
				model = e["model"] or "-"
				use_ipaddr = "y" if e["use_ipaddr"] else "-"
				net_force_ipaddr = "y" if e["net_force_ipaddr"] else "-"
				lines.append(
					f"| {fname} | {model} | {e['ipaddr']} | {use_ipaddr} | {net_force_ipaddr} |\n"
				)
			lines.append("\n")
			continue

		lines.append("| Filename | Model | IPADDR | USE_IPADDR | NET_FORCE_IPADDR |\n")
		lines.append("|---|---|---|---|---|\n")
		if default_count:
			lines.append(
				f"| _default-only configs omitted_ | {default_count} file(s) use `{DEFAULT_IPADDR}` | - | - | - |\n"
			)
		for e in non_default:
			fname = f'`{e["filename"]}`'
			model = e["model"] or "-"
			use_ipaddr = "y" if e["use_ipaddr"] else "-"
			net_force_ipaddr = "y" if e["net_force_ipaddr"] else "-"
			lines.append(
				f"| {fname} | {model} | {e['ipaddr']} | {use_ipaddr} | {net_force_ipaddr} |\n"
			)
		lines.append("\n")

	with open(outpath, "w", encoding="utf-8") as f:
		f.writelines(lines)

	return outpath


def main():
	script_dir = os.path.dirname(os.path.abspath(__file__))
	default_config_dir = os.path.join(script_dir, "..", "..", "uboot-mtk-20250711", "configs")
	default_output_dir = os.path.join(script_dir, "..")
	default_outname = "ipaddr_summary.md"

	config_dir = default_config_dir
	outname = default_outname

	if len(sys.argv) >= 2:
		config_dir = sys.argv[1]
	if len(sys.argv) >= 3:
		outname = sys.argv[2]

	entries = collect(config_dir)
	outpath = os.path.join(default_output_dir, outname)
	render_md(entries, outpath)
	print(f"Exported to: {outpath}, processed {len(entries)} configuration files.")


if __name__ == "__main__":
	main()
