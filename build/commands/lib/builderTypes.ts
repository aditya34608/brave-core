// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// TODO(http://brave.dev/b/56449): Experimental implementation for integration
// with our bots generator.

// A builder's spec: what it builds and syncs for, as bots declares it. Its GN
// args stay private to `bots gen`.
export interface BuilderSpec {
  builder_group: string
  chromium_config: {
    target_platform: string
    host_platform?: string | undefined
    build_config?: string | undefined
    target_arch?: string | undefined
    target_bits?: number | undefined
  }
  gclient_config: {
    // What the checkout syncs besides the host, which gclient always syncs.
    target_os: string[]
    target_cpu: string[]
    // What the builder adds to the chromium solution's, on top of
    // package.json's.
    custom_deps?: Record<string, string | null> | undefined
    custom_vars?: Record<string, string | boolean> | undefined
  }
}

export interface Builder {
  name: string
  spec: BuilderSpec
  // TODO(http://brave.dev/b/56449): GN's `target_os` and `target_cpu`, the
  // only GN args read from the builder's `gn-args.json`, until `init`/`sync`
  // no longer need a target to build for.
  buildTarget: { targetOS: string; targetArch: string }
  targets: { compile: string[]; tests: string[] }
}
