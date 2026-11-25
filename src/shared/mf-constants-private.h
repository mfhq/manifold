/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

/*
 * Names and paths that are shared between several Manifold programs, and
 * that do not depend on the build configuration.
 *
 * NOTE: This header must not use GLib.
 */

/* the directory with the programs of an application in its mount tree */
#define MF_TREE_APP_BIN_DIR "/app/bin"

/* the file that describes how a mount tree was assembled, relative to the root of the tree */
#define MF_TREE_ENVIRONMENT_RECORD "run/manifold/environment.json"

/* the directory of a runtime with its environment files, relative to the root of the runtime */
#define MF_TREE_ENVIRONMENT_DIR "usr/lib/manifold/environment.d"

/* the directory in a mount tree with the links to the programs of the host */
#define MF_HOST_BIN_DIR "/run/host-bin"

/* the program in a mount tree that asks the host to run a program, it is provided by the core */
#define MF_HOST_RUN_PATH "/usr/bin/mfhost-run"

/* the directory with the links to the programs in a user's own directories, relative
 * to the runtime directory of the user */
#define MF_HOST_USER_BIN_SUBDIR "manifold/host-bin"

/* the environment variable with the PATH in which the host looks up programs for applications */
#define MF_HOST_PATH_VARIABLE "MANIFOLD_HOST_PATH"
