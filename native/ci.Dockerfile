# ceyx CI guard container -- Phase 1 of the four-phase CI migration.
#
# WHAT THIS FILE IS FOR, precisely: it is the SINGLE SOURCE OF THE PINNED
# IMAGE IDENTITY. `native/scripts/ci/guards.py` parses the `FROM` line below
# and renders the `docker run` command string from it; nothing else in this
# repo names an image. Change the digest here and both the local gate and
# the CI step move together, because there is exactly one string and one
# renderer producing it -- which is the property the Phase 1 contract asks
# for. Two command strings that "agree by construction in the author's head"
# is the defect that requirement exists to remove.
#
# WHY A DIGEST AND NEVER A TAG: `python:3.11` is a MUTABLE POINTER. The
# Docker Hub tag is re-pushed on every patch/base-OS refresh, so a gate
# pinned to the tag silently changes what it tests between two runs that
# report the same string. `python@sha256:...` is content-addressed and
# cannot change under the gate. To move the pin, run the re-pin procedure at
# the bottom of this file and commit the diff -- deliberately a reviewed
# edit, not an automatic resolution.
#
# WHY THIS IMAGE AND NOT `-slim`: probed, not assumed (evidence:
# tmp/verify/impl-p1-docker/base-image-probe.txt and
# base-image-probe-full.txt). `python:3.11-slim` has NO `git` binary, and
# three of the guards shell out to git (`git ls-files` in
# check_cmake_sources_tracked.py, and the ignore/tracked queries in
# check_expected_additions.py and check_shell_prohibition.py), so on -slim
# they would fail for a reason that has nothing to do with the property they
# guard -- a false red, which is as corrosive as a false green. The full
# `python:3.11` image ships git 2.47.3. Using it as-is also means the gate
# needs NO `docker build` at all, which is what lets the run command carry a
# REAL, shareable digest; see "WHY THERE IS NO BUILD STEP" below.
#
# WHAT IS DELIBERATELY ABSENT, and must stay absent:
#
#   PyYAML. This is not an oversight, it is the point of the container.
#   GitHub's runner images do not ship PyYAML for the pinned
#   `actions/setup-python` interpreter. This campaign already paid for that
#   fact once: nine tests came off the roster and CI went red because a
#   guard imported `yaml` and passed on a laptop where it happened to be
#   installed. A gate that runs on a machine with PyYAML present cannot
#   observe that class of defect at all. Verified absent in this image:
#   `python3 -c "import yaml"` -> ModuleNotFoundError (probe artifact above).
#
#   Any `pip install`. Every line added here is a way for the container to
#   stop resembling the runner. If a guard needs a third-party package, that
#   is a finding to take to the campaign lead, not a line to add to this
#   file -- the guards are supposed to run on the stdlib the runner actually
#   has.
#
# WHY THERE IS NO BUILD STEP (named, not silent -- this is a real design
# trade and the reader deserves it stated): a locally-`docker build`-ed
# image has an image ID that is a function of THIS machine's build, not a
# digest a CI runner could ever name. Referencing such an image in the run
# command would force the local string and the CI string to differ in
# exactly the field the contract most cares about, or else to fall back to a
# shared mutable TAG, which defeats the pin. So the gate runs the pinned
# base image directly and the entrypoint is the repo's own checked-out code
# (bind-mounted read-only). This Dockerfile is therefore a DIGEST LEDGER and
# an exact reproducer of the gate environment, not an artifact the gate
# builds on every run. `docker build -f native/ci.Dockerfile .` still works
# and still yields the same environment, for a human who wants a shell in
# it.
FROM python@sha256:8ce0c4b7bad2a0939d3fe311e30f1f12b491b0ba335fe1f2daa1b99894588503

# The bind mount is read-only, so Python must not try to write .pyc files
# next to the sources it imports. Set here as well as in the run command so
# an interactive `docker build` + `docker run` of this image behaves like
# the gate does.
ENV PYTHONDONTWRITEBYTECODE=1

# The gate bind-mounts the repo at /w; match that here so a hand-built image
# lands a human in the same working directory the guards see.
WORKDIR /w

# RE-PIN PROCEDURE (reviewed edit, never automated):
#   1. docker pull --platform linux/amd64 python:3.11
#   2. read the `Digest: sha256:...` line it prints
#   3. replace the digest in the FROM line above with it
#   4. re-run the probe and confirm BOTH still hold, because the whole gate
#      rests on them:
#        docker run --rm --platform linux/amd64 python@sha256:<new> \
#          sh -c 'git --version; python3 -c "import yaml"'
#      expected: a git version prints, AND the yaml import raises
#      ModuleNotFoundError. A new base image that starts shipping PyYAML
#      must NOT be adopted -- it would make the gate blind to the exact
#      defect class it was built for.
#   5. re-run `python3 native/scripts/ci.py guards --docker` locally and
#      confirm the printed GUARDS_DOCKER_CMD line carries the new digest.
