from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
REF_PREFIX = "refs/checkpoints/"


class GitCommandError(RuntimeError):
    """Raised when a git subprocess fails."""


@dataclass(frozen=True)
class Checkpoint:
    ref: str
    short_ref: str
    commit: str
    created_at: str
    subject: str

    @property
    def name(self) -> str:
        prefix = "checkpoint: "
        if self.subject.startswith(prefix):
            return self.subject[len(prefix):]
        return self.subject


def run_git(
    args: list[str],
    *,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        ["git", *args],
        cwd=REPO_ROOT,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    if check and result.returncode != 0:
        raise GitCommandError(format_git_error(args, result))
    return result


def format_git_error(
    args: list[str],
    result: subprocess.CompletedProcess[str],
) -> str:
    stderr = (result.stderr or "").strip()
    stdout = (result.stdout or "").strip()
    details = stderr or stdout or "git command failed"
    return f"git {' '.join(args)}: {details}"


def git_output(
    args: list[str],
    *,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> str:
    return run_git(args, env=env, check=check).stdout.strip()


def repo_has_head() -> bool:
    return run_git(["rev-parse", "--verify", "HEAD"], check=False).returncode == 0


def repo_is_dirty() -> bool:
    return bool(
        git_output(
            ["status", "--porcelain", "--untracked-files=all"],
            check=False,
        )
    )


def current_branch_name() -> str:
    branch = git_output(["branch", "--show-current"], check=False)
    return branch or "(detached)"


def slugify(text: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return slug or "checkpoint"


def ref_exists(ref: str) -> bool:
    result = run_git(["show-ref", "--verify", "--quiet", ref], check=False)
    return result.returncode == 0


def unique_ref_name(base_name: str) -> str:
    candidate = f"{REF_PREFIX}{base_name}"
    suffix = 2
    while ref_exists(candidate):
        candidate = f"{REF_PREFIX}{base_name}-{suffix}"
        suffix += 1
    return candidate


def unique_branch_name(base_name: str) -> str:
    candidate = base_name
    suffix = 2
    while ref_exists(f"refs/heads/{candidate}"):
        candidate = f"{base_name}-{suffix}"
        suffix += 1
    return candidate


def build_snapshot_tree() -> str:
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_index = Path(temp_dir) / "index"
        env = os.environ.copy()
        env["GIT_INDEX_FILE"] = str(temp_index)
        run_git(["add", "-A", "--", "."], env=env)
        return git_output(["write-tree"], env=env)


def create_checkpoint(name: str) -> Checkpoint:
    now = datetime.now().astimezone()
    tree = build_snapshot_tree()
    head = git_output(["rev-parse", "HEAD"]) if repo_has_head() else ""
    subject = f"checkpoint: {name}"
    body_lines = [
        f"Created-At: {now.isoformat(timespec='seconds')}",
        f"Base-Branch: {current_branch_name()}",
    ]
    if head:
        body_lines.append(f"Base-Commit: {head}")
    commit_args = ["commit-tree", tree]
    if head:
        commit_args.extend(["-p", head])
    commit_args.extend(["-m", subject, "-m", "\n".join(body_lines)])
    commit = git_output(commit_args)

    ref_basename = f"{now.strftime('%Y%m%d-%H%M%S')}-{slugify(name)}"
    ref = unique_ref_name(ref_basename)
    run_git(["update-ref", ref, commit])
    return Checkpoint(
        ref=ref,
        short_ref=ref[len(REF_PREFIX):],
        commit=commit,
        created_at=now.isoformat(timespec="seconds"),
        subject=subject,
    )


def list_checkpoints() -> list[Checkpoint]:
    output = git_output(
        [
            "for-each-ref",
            "--sort=-creatordate",
            (
                "--format=%(refname)%00%(refname:strip=2)%00"
                "%(objectname)%00%(creatordate:iso-strict)%00"
                "%(contents:subject)"
            ),
            REF_PREFIX,
        ],
        check=False,
    )
    if not output:
        return []

    checkpoints: list[Checkpoint] = []
    for line in output.splitlines():
        ref, short_ref, commit, created_at, subject = line.split("\0")
        checkpoints.append(
            Checkpoint(
                ref=ref,
                short_ref=short_ref,
                commit=commit,
                created_at=created_at,
                subject=subject,
            )
        )
    return checkpoints


def resolve_checkpoint(selector: str) -> Checkpoint:
    checkpoints = list_checkpoints()
    if not checkpoints:
        raise GitCommandError("No checkpoints exist yet.")

    lowered = selector.lower()
    exact_matches = [
        checkpoint
        for checkpoint in checkpoints
        if selector in {checkpoint.ref, checkpoint.short_ref, checkpoint.commit}
        or checkpoint.commit.startswith(selector)
    ]
    if len(exact_matches) == 1:
        return exact_matches[0]
    if len(exact_matches) > 1:
        raise GitCommandError(
            render_ambiguous_checkpoint_error(selector, exact_matches)
        )

    fuzzy_matches = [
        checkpoint
        for checkpoint in checkpoints
        if lowered in checkpoint.short_ref.lower() or lowered in checkpoint.name.lower()
    ]
    if len(fuzzy_matches) == 1:
        return fuzzy_matches[0]
    if len(fuzzy_matches) > 1:
        raise GitCommandError(
            render_ambiguous_checkpoint_error(selector, fuzzy_matches)
        )
    raise GitCommandError(f"No checkpoint matched '{selector}'.")


def render_ambiguous_checkpoint_error(
    selector: str,
    checkpoints: list[Checkpoint],
) -> str:
    lines = [f"Checkpoint selector '{selector}' matched multiple checkpoints:"]
    for checkpoint in checkpoints:
        lines.append(f"  - {checkpoint.short_ref} ({checkpoint.name})")
    return "\n".join(lines)


def restore_checkpoint(
    selector: str,
    *,
    detached: bool,
) -> tuple[Checkpoint, Checkpoint | None, str]:
    target = resolve_checkpoint(selector)
    autosave: Checkpoint | None = None
    if repo_is_dirty():
        autosave = create_checkpoint(
            f"autosave before restore {target.short_ref}"
        )

    if detached:
        run_git(["switch", "--detach", "--discard-changes", target.commit])
        return target, autosave, "(detached)"

    branch_name = unique_branch_name(
        f"checkpoint/{slugify(target.short_ref)}"
    )
    run_git(
        ["switch", "--discard-changes", "-c", branch_name, target.commit]
    )
    return target, autosave, branch_name


def delete_checkpoint(selector: str) -> Checkpoint:
    checkpoint = resolve_checkpoint(selector)
    run_git(["update-ref", "-d", checkpoint.ref])
    return checkpoint


def command_save(args: argparse.Namespace) -> int:
    checkpoint = create_checkpoint(" ".join(args.name).strip())
    print(f"Saved checkpoint {checkpoint.short_ref}")
    print(f"Commit: {checkpoint.commit}")
    return 0


def command_list(_: argparse.Namespace) -> int:
    checkpoints = list_checkpoints()
    if not checkpoints:
        print("No checkpoints saved yet.")
        return 0

    for checkpoint in checkpoints:
        print(
            f"{checkpoint.short_ref}\t{checkpoint.commit[:12]}"
            f"\t{checkpoint.created_at}\t{checkpoint.name}"
        )
    return 0


def command_restore(args: argparse.Namespace) -> int:
    target, autosave, branch_name = restore_checkpoint(
        args.selector,
        detached=args.detached,
    )
    if autosave:
        print(f"Saved current state as {autosave.short_ref}")
    if args.detached:
        print(f"Restored {target.short_ref} in detached HEAD")
    else:
        print(f"Restored {target.short_ref} on branch {branch_name}")
    return 0


def command_delete(args: argparse.Namespace) -> int:
    checkpoint = delete_checkpoint(args.selector)
    print(f"Deleted checkpoint {checkpoint.short_ref}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Save and restore local git-backed checkpoints without "
            "creating normal commits."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    save_parser = subparsers.add_parser(
        "save",
        help="Save the current repo state as a named checkpoint.",
    )
    save_parser.add_argument("name", nargs="+", help="Human-readable checkpoint name.")
    save_parser.set_defaults(func=command_save)

    list_parser = subparsers.add_parser("list", help="List saved checkpoints.")
    list_parser.set_defaults(func=command_list)

    restore_parser = subparsers.add_parser(
        "restore",
        help="Restore a checkpoint by switching to it on a fresh branch.",
    )
    restore_parser.add_argument(
        "selector",
        help="Checkpoint name, ref suffix, or commit SHA.",
    )
    restore_parser.add_argument(
        "--detached",
        action="store_true",
        help="Restore into detached HEAD instead of creating a branch.",
    )
    restore_parser.set_defaults(func=command_restore)

    delete_parser = subparsers.add_parser(
        "delete",
        help="Delete a saved checkpoint.",
    )
    delete_parser.add_argument(
        "selector",
        help="Checkpoint name, ref suffix, or commit SHA.",
    )
    delete_parser.set_defaults(func=command_delete)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.func(args)
    except GitCommandError as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
