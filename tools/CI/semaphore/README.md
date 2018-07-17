## `semaphoreci` based NEMU Continuous Integration

This is a short guide for the `semaphoreci` based NEMU automated CI.

### Why `semaphoreci` ?

[`semaphoreci`](https://semaphoreci.com/) is the only public, github bound, hosted CI
that provides nested virtualization access.
Since the NEMU CI is mostly based around launching and testing VM instances, getting
VT access is a mandatory requirement.

### Setup

`semaphoreci` provides 2 types of instances: Standard and [Docker native](https://semaphoreci.com/docs/docker/setting-up-continuous-integration-for-docker-project.html) ones. Nested virtualization is only accessible through
the latter one.

As such, one should make sure to select the Docker native platform when
running the NEMU CI on semaphore.

### `semaphoreci` jobs

There currently are 2 NEMU scripts for `semaphoreci`:

1. `build_aarch64.sh` does an ARM64 build inside ccloudvm
2. `build_x86_64.sh` does an IA build natively
3. `minimal-ci.sh` to run the NEMU minimal CI script on IA.

The IA build and CI testing and ARM64 build happen in parallel on Semaphore.

### TODO

* Parallel runs: Run one job per image to be tested

