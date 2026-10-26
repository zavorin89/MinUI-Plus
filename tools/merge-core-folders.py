#!/usr/bin/env python3
"""Moves games out of an alternate core's own system folder (eg. "Super Nintendo (SFC10)") into the main one
("Super Nintendo (SFC)") and sets each moved game to keep using that core (MENU + Y in MinUI changes it later).

Everything that belongs to a game moves with it: box art, in-game saves, save states and their pictures,
per-game settings and borders, and its entries in Recents, Favorites, play time, the game switcher and a
pending power-off resume. Nothing is lost: when the main folder already has a file by the same name (eg. an old save
from before the game was moved out), the moved game's file takes its place and the old one is renamed .before-merge.

Run it with the card mounted. It only shows what it would do unless given --apply:
    python3 tools/merge-core-folders.py "/Volumes/MIN UI" SFC10:SFC SNESX:SFC MGBA:GBA
    python3 tools/merge-core-folders.py "/Volumes/MIN UI" SFC10:SFC SNESX:SFC MGBA:GBA --apply
Every move is logged to .userdata/shared/.minui/merge-log.txt (old path, tab, new path) so it can be undone.
"""
import os
import re
import sys

PLATFORM = "my282"


def tag_of(folder):
    match = re.search(r"\(([^()]*)\)\s*$", folder)
    return match.group(1) if match else None


def system_folder(roms, tag):
    for name in sorted(os.listdir(roms)):
        if not name.startswith(".") and os.path.isdir(os.path.join(roms, name)) and tag_of(name) == tag:
            return name
    return None


def is_file_of(file, name):  # the name, then '.' or '-' (eg. "Zelda.sfc.sav", "Zelda.sfc-a30.cfg", not "Zelda.sfc2.sav")
    return file.startswith(name) and len(file) > len(name) and file[len(name)] in ".-"


class Merge:
    def __init__(self, card, apply):
        self.card = card
        self.apply = apply
        self.moves = []  # (old, new)
        self.problems = []
        self.renames = {}  # old card-relative game path -> new
        self.cores = []  # (new card-relative game path, alternate tag)

    def move(self, old, new):
        if not os.path.exists(old):
            return
        if os.path.exists(new):
            # the moved game's file is the one in use, the one already there is kept beside it as a backup
            backup = new + ".before-merge"
            if os.path.exists(backup):
                self.problems.append(f"already there (and a backup too), left alone: {os.path.relpath(old, self.card)}")
                return
            self.problems.append(f"replaced, the old one is now {os.path.relpath(backup, self.card)}")
            self.moves.append((new, backup))
            if self.apply:
                os.rename(new, backup)
        self.moves.append((old, new))
        if self.apply:
            os.makedirs(os.path.dirname(new), exist_ok=True)
            os.rename(old, new)

    def move_matching(self, old_dir, new_dir, names, only=None):
        if not os.path.isdir(old_dir):
            return
        for file in sorted(os.listdir(old_dir)):
            if file.startswith(".") or (only and not only(file)):
                continue
            if any(is_file_of(file, name) for name in names):
                self.move(os.path.join(old_dir, file), os.path.join(new_dir, file))

    def merge(self, alt, main):
        roms = os.path.join(self.card, "Roms")
        alt_folder, main_folder = system_folder(roms, alt), system_folder(roms, main)
        if not alt_folder:
            print(f"{alt}: no Roms folder, nothing to move")
            return
        if not main_folder:
            self.problems.append(f"{alt}: no Roms folder for {main} to move its games into")
            return
        alt_dir, main_dir = os.path.join(roms, alt_folder), os.path.join(roms, main_folder)
        shared = os.path.join(self.card, ".userdata", "shared")
        device = os.path.join(self.card, ".userdata", PLATFORM)
        cores = [d[len(alt) + 1:] for d in os.listdir(shared) if d.startswith(alt + "-") and os.path.isdir(os.path.join(shared, d))]
        cores += [d[len(alt) + 1:] for d in os.listdir(device) if d.startswith(alt + "-") and os.path.isdir(os.path.join(device, d))]
        cores = sorted(set(cores))

        games = [g for g in sorted(os.listdir(alt_dir)) if not g.startswith(".") and g != "map.txt"]
        print(f"{alt_folder} -> {main_folder}: {len(games)} games")
        for game in games:
            old_rom, new_rom = os.path.join(alt_dir, game), os.path.join(main_dir, game)
            if os.path.exists(new_rom):
                self.problems.append(f"{game}: {main_folder} already has a game by that name, left in {alt_folder}")
                continue
            names = [game]  # saves go by the launched file's name, a multi-disc game's by its .m3u and each disc
            launched = game
            if os.path.isdir(old_rom):
                files = os.listdir(old_rom)
                m3u = game + ".m3u"
                if m3u in files:
                    launched = os.path.join(game, m3u)
                    names = [m3u] + [f for f in files if f != m3u]
            self.move(old_rom, new_rom)
            self.move(os.path.join(alt_dir, ".res", game + ".png"), os.path.join(main_dir, ".res", game + ".png"))
            self.move_matching(os.path.join(self.card, "Saves", alt), os.path.join(self.card, "Saves", main), names)
            self.move_matching(os.path.join(shared, ".minui", alt), os.path.join(shared, ".minui", main), names)
            self.move_matching(os.path.join(self.card, "Borders", alt), os.path.join(self.card, "Borders", main), names)
            for core in cores:
                self.move_matching(os.path.join(shared, f"{alt}-{core}"), os.path.join(shared, f"{main}-{core}"), names)
                self.move_matching(os.path.join(device, f"{alt}-{core}"), os.path.join(device, f"{main}-{core}"), names,
                                   only=lambda f: f.endswith(".cfg"))
            old_rel, new_rel = f"/Roms/{alt_folder}/{launched}", f"/Roms/{main_folder}/{launched}"
            self.renames[old_rel] = new_rel
            self.cores.append((new_rel, alt))

        # the core's own settings for the system (not any one game's) go along when the main folder has none yet
        for core in cores:
            old_dir, new_dir = os.path.join(device, f"{alt}-{core}"), os.path.join(device, f"{main}-{core}")
            if os.path.isdir(old_dir) and not os.path.isdir(new_dir):
                for file in sorted(os.listdir(old_dir)):
                    if file.endswith(".cfg") and not any(is_file_of(file, g) for g in games):
                        self.copy(os.path.join(old_dir, file), os.path.join(new_dir, file))

        map_path = os.path.join(alt_dir, "map.txt")
        if os.path.exists(map_path):
            self.problems.append(f"{alt_folder}/map.txt (custom names) wasn't merged: copy its lines into {main_folder}/map.txt")

    def copy(self, old, new):
        self.moves.append((old, new + "  (copied)"))
        if self.apply:
            os.makedirs(os.path.dirname(new), exist_ok=True)
            with open(old, "rb") as src, open(new, "wb") as dst:
                dst.write(src.read())

    def rewrite_lists(self):
        minui = os.path.join(self.card, ".userdata", "shared", ".minui")
        for name in ("recent.txt", "favorites.txt", "playtime.txt", "switcher.txt", "auto_resume.txt"):
            path = os.path.join(minui, name)
            if not os.path.exists(path):
                continue
            with open(path, encoding="utf-8", errors="surrogateescape") as file:
                text = file.read()
            lines, changed = [], 0
            for line in text.split("\n"):
                game, sep, rest = line.partition("\t")
                for old, new in self.renames.items():
                    if game == old or game.startswith(old + "/"):  # also a disc inside a moved multi-disc folder
                        game = new + game[len(old):]
                        changed += 1
                        break
                lines.append(game + sep + rest)
            if changed:
                print(f"{name}: {changed} entries updated")
                if self.apply:
                    with open(path, "w", encoding="utf-8", errors="surrogateescape") as file:
                        file.write("\n".join(lines))

        path = os.path.join(minui, "cores.txt")
        kept = []
        if os.path.exists(path):
            with open(path, encoding="utf-8", errors="surrogateescape") as file:
                kept = [l for l in file.read().split("\n") if l and l.partition("\t")[0] not in {g for g, _ in self.cores}]
        for game, tag in self.cores:
            print(f"core: {game} -> {tag}")
        if self.apply and self.cores:
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as file:
                file.write("\n".join(kept + [f"{g}\t{t}" for g, t in self.cores]) + "\n")
            with open(os.path.join(minui, "merge-log.txt"), "a", encoding="utf-8", errors="surrogateescape") as log:
                for old, new in self.moves:
                    log.write(f"{os.path.relpath(old, self.card)}\t{os.path.relpath(new, self.card)}\n")

    def remove_empty(self):
        roms = os.path.join(self.card, "Roms")
        for old_rel in list(self.renames):
            folder = os.path.join(roms, old_rel.split("/")[2])
            res = os.path.join(folder, ".res")
            for path in (res, folder):
                if os.path.isdir(path) and not [f for f in os.listdir(path) if not f.startswith("._") and f != ".DS_Store"]:
                    print(f"removing empty {os.path.relpath(path, self.card)}")
                    if self.apply:
                        for junk in os.listdir(path):
                            os.remove(os.path.join(path, junk))
                        os.rmdir(path)


def main():
    args = [a for a in sys.argv[1:] if a != "--apply"]
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)
    merge = Merge(args[0], "--apply" in sys.argv)
    for pair in args[1:]:
        alt, main_tag = pair.split(":")
        merge.merge(alt, main_tag)
    merge.rewrite_lists()
    merge.remove_empty()
    print(f"{len(merge.moves)} files {'moved' if merge.apply else 'to move'}")
    for move in merge.moves:
        print(f"  {os.path.relpath(move[0], merge.card)}  ->  {os.path.relpath(move[1], merge.card)}")
    for problem in merge.problems:
        print(f"NOTE: {problem}")
    if not merge.apply:
        print("dry run: nothing changed (add --apply to do it)")


main()
