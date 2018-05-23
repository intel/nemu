# Contributing to the NEMU project

* [Code of Conduct](#code-of-conduct)
* [Certificate of Origin](#certificate-of-origin)
* [Pull requests](#pull-requests)
    * [Before starting work on a PR](#before-starting-work-on-a-pr)
    * [Normal PR workflow](#normal-pr-workflow)
* [Patch format](#patch-format)
    * [General format](#general-format)
    * [Subsystem](#subsystem)
    * [Advice](#advice)
* [Reviews](#reviews)
* [Project maintainers](#project-maintainers)
* [Closing issues](#closing-issues)

NEMU is an open source project released under the [GNU General Public License,
version 2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).

## Code of Conduct

All contributors must agree to the project [code of conduct](CODE_OF_CONDUCT.md).

## Certificate of Origin

In order to get a clear contribution chain of trust we use the [signed-off-by
language](https://ltsi.linuxfoundation.org/software/signed-off-process/)
used by the Linux kernel project.

## Pull requests

All the repositories accept contributions by a GitHub Pull request (PR).

### Before starting work on a PR

We welcome all contributions but to minimize the chance of multiple people
working on the same feature or bug fixes (yes, it has happened), we recommend
strongly that you raise a GitHub issue **before** you start working on a PR.

If you are a new contributor, you cannot assign the issue to yourself. In this
case, raise the issue and add a comment stating that you intend to work on the
issue yourself. This gives the team visibility of the work you plan to do.

The other advantage to raising the issue at the outset is that our process requires an
issue to be associated with every PR (see [patch format](#patch-format)).

### Normal PR workflow

Github has a basic introduction to the PR process
[here](https://help.github.com/articles/using-pull-requests/).

When submitting your PR, treat the PR message the same
you would a patch message, including pre-fixing the title with a subsystem
name.

By default, GitHub copies the message from your first patch, which is often
appropriate. However, ensure your message is accurate and complete for the
entire PR because it ends up in the Git log as the merge message.

Your PR might get feedback and comments, and require rework. The recommended
procedure for reworking is to redo your branch to a clean state and "force
push" it to your GitHub branch. A "forced push" is adequate, which is
reflected in the online comment history. Do not pile patches on patches to
rework your branch. You should rework any relevant information from the GitHub
comment section into your patch set because your patches are documented in the
Git log, not the comment section.

For more information on GitHub "force push" workflows see "[Why and how to
correctly amend GitHub pull
requests](http://blog.adamspiers.org/2015/03/24/why-and-how-to-correctly-amend-github-pull-requests/)".

Your PR can contain more than one patch. Use as many patches as necessary to
implement the request. Each PR should only cover one topic. If you mix up
different items in your patches or PR, they will likely need to be reworked.

## Patch format

### General format

Beside the `Signed-off-by` footer, we expect each patch to comply with the
following format:

```
subsystem: One line change summary

More detailed explanation of your changes (why and how)
that spans as many lines as required.

A "Fixes #XXX" comment listing the GitHub issue this change resolves.
This comment is required for the main patch in a sequence. See the following examples.

Signed-off-by: <contributor@foo.com>
```

The body of the message is not a continuation of the subject line and is not
used to extend the subject line beyond its character limit.
The subject line is a complete sentence and the body is a complete, standalone paragraph.

### Subsystem

The "subsystem" describes the area of the code that the change applies to. It does not have to match a particular directory name in the source tree because it is a "hint" to the reader. The subsystem is generally a single word. Although the subsystem must be specified, it is not validated. The author decides what is a relevant subsystem for each patch.

To see the subsystem values chosen for existing commits:

```
$ git log --no-merges --pretty="%s" | cut -d: -f1 | sort -u
```

### Advice

It is recommended that each of your patches fixes one thing. Smaller patches
are easier to review, more likely accepted and merged, and problems are more
likely to be identified during review.

A PR can contain multiple patches. These patches should generally be related to the [main patch](#main-patch) and the overall goal of the PR. However, it is also acceptable to include additional or [supplementary patches](#supplementary-patch) for things such as:

- Formatting (or whitespace) fixes
- Comment improvements
- Tidy up work
- Refactoring to simplify the codebase

## Reviews

Before your PRs are merged into the main code base, they are reviewed. We
encourage anybody to review any PR and leave feedback.

See the [PR review guide](PR-Review-Guide.md) for tips on performing a careful review.

We use an "acknowledge" system for people to note if they agree or disagree
with a PR. We utilize some automated systems that can spot common acknowledge
patterns, which include placing any of these **at the beginning of a comment
line**:

 - LGTM
 - lgtm
 - +1
 - Approve

## Project maintainers

The NEMU project maintainers are the people accepting or
rejecting any PR. Although [anyone can review PRs](#reviews), only the
acknowledgement (or "ack") from an Approver counts towards the approval of a PR.

The project uses the [Pull Approve](https://pullapprove.com) service meaning
each repository contains a top level `.pullapprove.yml` configuration file. This
file lists the GitHub team used for approvals.

The minimum approval requirements are:

- Two approvals from the approval team listed in the configuration file.

See the `.pullapprove.yml` configuration files for full details.

## Closing issues

Our tooling requires adding a `Fixes` comment to at least one commit in the PR,
which triggers GitHub to automatically close the issue once the PR is merged:

```
hw/i386: Use virtio-net as default NIC 

This enables some progress with unit tests. 

Fixes #123

Signed-off-by: Rob Bradford <robert.bradford@intel.com> 
```

The issue is automatically closed by GitHub when the [commit
message](https://help.github.com/articles/closing-issues-via-commit-messages/)
is parsed.
