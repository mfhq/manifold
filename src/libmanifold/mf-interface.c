/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include "mf-interface-private.h"

#include <systemd/sd-varlink.h>

/*
 * Definition of the org.freedesktop.manifold Varlink interface.
 */

/* clang-format off */

static SD_VARLINK_DEFINE_ENUM_TYPE(
    ComponentKind,
    SD_VARLINK_FIELD_COMMENT("The root filesystem that all other layers are built on."),
    SD_VARLINK_DEFINE_ENUM_VALUE(core),
    SD_VARLINK_FIELD_COMMENT("A platform many applications share, based on a strict core version."),
    SD_VARLINK_DEFINE_ENUM_VALUE(platform),
    SD_VARLINK_FIELD_COMMENT("The packages or components that an application needs in addition to the layer below."),
    SD_VARLINK_DEFINE_ENUM_VALUE(kit),
    SD_VARLINK_FIELD_COMMENT("An application."),
    SD_VARLINK_DEFINE_ENUM_VALUE(app),
    SD_VARLINK_FIELD_COMMENT("A plugin of an application."),
    SD_VARLINK_DEFINE_ENUM_VALUE(plugin));

static SD_VARLINK_DEFINE_ENUM_TYPE(
    InstallMode,
    SD_VARLINK_FIELD_COMMENT("The component is kept until it is removed explicitly."),
    SD_VARLINK_DEFINE_ENUM_VALUE(manual),
    SD_VARLINK_FIELD_COMMENT("The component is removed once it is not needed anymore."),
    SD_VARLINK_DEFINE_ENUM_VALUE(auto));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Component,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The kind of the component."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(kind, ComponentKind, 0),
    SD_VARLINK_FIELD_COMMENT("Version of the component."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Revision of the version (Automatically set by the builder, or by a repository)."),
    SD_VARLINK_DEFINE_FIELD(revision, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("Whether this is the SDK image of the component."),
    SD_VARLINK_DEFINE_FIELD(sdk, SD_VARLINK_BOOL, 0),
    SD_VARLINK_FIELD_COMMENT("The identity hash of the image, as a hex encoded string."),
    SD_VARLINK_DEFINE_FIELD(identity, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The dm-verity root hash of the image, as a hex encoded string."),
    SD_VARLINK_DEFINE_FIELD(verityHash, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Whether this is the active version of the component, the one that is launched."),
    SD_VARLINK_DEFINE_FIELD(current, SD_VARLINK_BOOL, 0),
    SD_VARLINK_FIELD_COMMENT("Whether the component is a potential target for automatic removal."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(installMode, InstallMode, 0),
    SD_VARLINK_FIELD_COMMENT("The origin of the component: '<remote>-<channel>' for a component from a "
                             "remote, or 'manifold-local' for one from a bundle."),
    SD_VARLINK_DEFINE_FIELD(origin, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ENUM_TYPE(
    Action,
    SD_VARLINK_FIELD_COMMENT("A component is installed."),
    SD_VARLINK_DEFINE_ENUM_VALUE(install),
    SD_VARLINK_FIELD_COMMENT("A component is removed on request."),
    SD_VARLINK_DEFINE_ENUM_VALUE(remove),
    SD_VARLINK_FIELD_COMMENT("An unused component is removed."),
    SD_VARLINK_DEFINE_ENUM_VALUE(cleanup),
    SD_VARLINK_FIELD_COMMENT("The metadata of a remote is downloaded."),
    SD_VARLINK_DEFINE_ENUM_VALUE(refresh));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    ItemProgress,
    SD_VARLINK_FIELD_COMMENT("The action that is performed on the subject."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(action, Action, 0),
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that is acted on."),
    SD_VARLINK_DEFINE_FIELD(subject, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Version of the component, with its revision: '<version>-<rev>'."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("Progress of this item in percent, from 0 to 100."),
    SD_VARLINK_DEFINE_FIELD(percent, SD_VARLINK_FLOAT, 0),
    SD_VARLINK_FIELD_COMMENT("Bytes of the image copied so far, while an image is copied."),
    SD_VARLINK_DEFINE_FIELD(bytesDone, SD_VARLINK_INT, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("Size of the image in bytes, while an image is copied."),
    SD_VARLINK_DEFINE_FIELD(bytesTotal, SD_VARLINK_INT, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Progress,
    SD_VARLINK_FIELD_COMMENT("Progress of the whole operation in percent, from 0 to 100."),
    SD_VARLINK_DEFINE_FIELD(percent, SD_VARLINK_FLOAT, 0),
    SD_VARLINK_FIELD_COMMENT("The item that is being worked on right now."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(item, ItemProgress, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Result,
    SD_VARLINK_FIELD_COMMENT("The components that were installed."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(installed, Component, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("The components that were removed, including dependencies that were no longer needed."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(removed, Component, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("The verity hashes of the images that were removed."),
    SD_VARLINK_DEFINE_FIELD(removedImages, SD_VARLINK_STRING, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("Bytes of disk space that the operation used."),
    SD_VARLINK_DEFINE_FIELD(bytesAdded, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("Bytes of disk space that the operation freed."),
    SD_VARLINK_DEFINE_FIELD(bytesFreed, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("Problems that occurred, but did not make the operation fail."),
    SD_VARLINK_DEFINE_FIELD(warnings, SD_VARLINK_STRING, SD_VARLINK_ARRAY));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Done,
    SD_VARLINK_FIELD_COMMENT("The components that the operation changed, in their new state."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(components, Component, SD_VARLINK_ARRAY));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    RemoteInfo,
    SD_VARLINK_FIELD_COMMENT("The ID of the repository, which is the ID of its remote."),
    SD_VARLINK_DEFINE_FIELD(id, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The https URL of the repository, without a '/' at its end."),
    SD_VARLINK_DEFINE_FIELD(url, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("A short description of the repository."),
    SD_VARLINK_DEFINE_FIELD(summary, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The name of the publisher of the repository, if it has one."),
    SD_VARLINK_DEFINE_FIELD(publisherName, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The email address of the publisher, if it has one."),
    SD_VARLINK_DEFINE_FIELD(publisherEmail, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("When the root index of the repository expires, in ISO 8601 format."),
    SD_VARLINK_DEFINE_FIELD(validUntil, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The channels of the repository that have components for the architecture of this system."),
    SD_VARLINK_DEFINE_FIELD(channels, SD_VARLINK_STRING, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("The public key that signs the root index, in OpenSSH format without its comment."),
    SD_VARLINK_DEFINE_FIELD(publicKey, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The SHA256 fingerprint of the key, as 'ssh-keygen -l' shows it."),
    SD_VARLINK_DEFINE_FIELD(fingerprint, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ENUM_TYPE(
    TransactionFlag,
    SD_VARLINK_FIELD_COMMENT("The service may interact with the user, e.g. to ask for authorization of an operation."),
    SD_VARLINK_DEFINE_ENUM_VALUE(allowInteractive));

#define MF_TRANSACTION_FLAGS_INPUT \
    SD_VARLINK_FIELD_COMMENT("Flags that change the behavior of the operation."), \
    SD_VARLINK_DEFINE_INPUT_BY_TYPE(flags, TransactionFlag, SD_VARLINK_ARRAY | SD_VARLINK_NULLABLE)

#define MF_OPERATION_OUTPUT \
    SD_VARLINK_FIELD_COMMENT("Progress of the operation. Only set in the intermediate replies of a call with the 'more' flag."), \
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(progress, Progress, SD_VARLINK_NULLABLE), \
    SD_VARLINK_FIELD_COMMENT("Result of the operation. Only set in the final reply."), \
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(result, Result, SD_VARLINK_NULLABLE)

static SD_VARLINK_DEFINE_METHOD_FULL(
    InstallFile,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("Index of the file descriptor of the bundle that should be installed."),
    SD_VARLINK_DEFINE_INPUT(bundleFileDescriptor, SD_VARLINK_INT, 0),
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD_FULL(
    InstallComponent,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component to install, in its newest version."),
    SD_VARLINK_DEFINE_INPUT(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The origin to install the component from, '<remote>-<channel>'. "
                             "Only needed if more than one origin has the component."),
    SD_VARLINK_DEFINE_INPUT(origin, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD_FULL(
    RemoveComponent,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component to remove."),
    SD_VARLINK_DEFINE_INPUT(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Version to remove, with all of its revisions, or '<version>-<rev>' to specify a revision explicitly. "
                             "All versions are removed if this is not set."),
    SD_VARLINK_DEFINE_INPUT(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD_FULL(
    Cleanup,
    SD_VARLINK_SUPPORTS_MORE,
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD_FULL(
    Refresh,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("The IDs of the remotes to refresh. All remotes are refreshed if this is not set."),
    SD_VARLINK_DEFINE_INPUT(remotes, SD_VARLINK_STRING, SD_VARLINK_ARRAY | SD_VARLINK_NULLABLE),
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD_FULL(
    Update,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("The components to update. Every component from a remote is updated if this is not set."),
    SD_VARLINK_DEFINE_INPUT(componentIds, SD_VARLINK_STRING, SD_VARLINK_ARRAY | SD_VARLINK_NULLABLE),
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD(
    InspectRemote,
    SD_VARLINK_FIELD_COMMENT("The https URL of the repository."),
    SD_VARLINK_DEFINE_INPUT(url, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The public key that has to sign the repository, in OpenSSH format. "
                             "If this is not set, the key that the repository serves is used."),
    SD_VARLINK_DEFINE_INPUT(publicKey, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    MF_TRANSACTION_FLAGS_INPUT,
    SD_VARLINK_FIELD_COMMENT("The description of the repository, and the key that signs it."),
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(remote, RemoteInfo, 0));

static SD_VARLINK_DEFINE_METHOD_FULL(
    AddRemote,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("The ID of the repository, as InspectRemote returned it. A repository "
                             "that has a different ID is not added."),
    SD_VARLINK_DEFINE_INPUT(remoteId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The https URL of the repository."),
    SD_VARLINK_DEFINE_INPUT(url, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The public key that signs the repository, in OpenSSH format. "
                             "The remote trusts this key."),
    SD_VARLINK_DEFINE_INPUT(publicKey, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The channels to install from. Only 'main' if this is not set."),
    SD_VARLINK_DEFINE_INPUT(channels, SD_VARLINK_STRING, SD_VARLINK_ARRAY | SD_VARLINK_NULLABLE),
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD(
    RemoveRemote,
    SD_VARLINK_FIELD_COMMENT("The ID of the remote to remove."),
    SD_VARLINK_DEFINE_INPUT(remoteId, SD_VARLINK_STRING, 0),
    MF_TRANSACTION_FLAGS_INPUT);

static SD_VARLINK_DEFINE_METHOD(
    MarkComponent,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component to mark."),
    SD_VARLINK_DEFINE_INPUT(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Version to mark, with all of its revisions, or '<version>-<rev>' to specify a revision too. "
                             "The current version is marked if this is not set."),
    SD_VARLINK_DEFINE_INPUT(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The install mode to record."),
    SD_VARLINK_DEFINE_INPUT_BY_TYPE(mode, InstallMode, 0),
    MF_TRANSACTION_FLAGS_INPUT,
    SD_VARLINK_FIELD_COMMENT("The versions that were marked."),
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(done, Done, 0));

/*
 * Every error of the interface has a message that describes the problem.
 * Errors about a component may also have fields with details on the
 * component, which are only set when the service knows them.
 */
#define MF_ERROR_MESSAGE_FIELD \
    SD_VARLINK_FIELD_COMMENT("Description of the problem."), \
    SD_VARLINK_DEFINE_FIELD(message, SD_VARLINK_STRING, 0)

static SD_VARLINK_DEFINE_ERROR(
    Failed,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    NotImplemented,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    NotFound,
    MF_ERROR_MESSAGE_FIELD,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that was not found."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The version that was not found, if a specific version was requested."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ERROR(
    InvalidBundle,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    WrongArchitecture,
    MF_ERROR_MESSAGE_FIELD,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that was built for another architecture."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The architecture the component was built for."),
    SD_VARLINK_DEFINE_FIELD(architecture, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The architecture of this system."),
    SD_VARLINK_DEFINE_FIELD(hostArchitecture, SD_VARLINK_STRING, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ERROR(
    VerificationFailed,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    MissingBase,
    MF_ERROR_MESSAGE_FIELD,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that is missing its base."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("Identifier of the base that is missing."),
    SD_VARLINK_DEFINE_FIELD(baseId, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("Version of the base that is missing."),
    SD_VARLINK_DEFINE_FIELD(baseVersion, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("Identity hash of the base that is missing."),
    SD_VARLINK_DEFINE_FIELD(identity, SD_VARLINK_STRING, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ERROR(
    AlreadyInstalled,
    MF_ERROR_MESSAGE_FIELD,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that is installed already."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The version that is installed already."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ERROR(
    ComponentInUse,
    MF_ERROR_MESSAGE_FIELD,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that is in use."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("Identifier of an installed component that builds on it."),
    SD_VARLINK_DEFINE_FIELD(usedBy, SD_VARLINK_STRING, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ERROR(
    InvalidManifest,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    Revoked,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    IdConflict,
    MF_ERROR_MESSAGE_FIELD,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that can not be installed."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The kind of the component that is installed with this identifier."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(kind, ComponentKind, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("Whether the component that is installed with this identifier is an SDK."),
    SD_VARLINK_DEFINE_FIELD(sdk, SD_VARLINK_BOOL, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ERROR(
    FileConflict,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    InvalidRepository,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    Expired,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    DownloadFailed,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    Ambiguous,
    MF_ERROR_MESSAGE_FIELD);

static SD_VARLINK_DEFINE_ERROR(
    RemoteExists,
    MF_ERROR_MESSAGE_FIELD);

SD_VARLINK_DEFINE_INTERFACE(
    org_freedesktop_manifold,
    MF_INTERFACE,
    SD_VARLINK_INTERFACE_COMMENT("Manages Manifold components."),

    SD_VARLINK_SYMBOL_COMMENT("The kind of a component."),
    &vl_type_ComponentKind,
    SD_VARLINK_SYMBOL_COMMENT("Whether a component is a potential target for automatic removal."),
    &vl_type_InstallMode,
    SD_VARLINK_SYMBOL_COMMENT("An installed component."),
    &vl_type_Component,
    SD_VARLINK_SYMBOL_COMMENT("An action that is performed on a component or an image."),
    &vl_type_Action,
    SD_VARLINK_SYMBOL_COMMENT("Progress of one item of an operation."),
    &vl_type_ItemProgress,
    SD_VARLINK_SYMBOL_COMMENT("Progress of an operation."),
    &vl_type_Progress,
    SD_VARLINK_SYMBOL_COMMENT("Result of an operation."),
    &vl_type_Result,
    SD_VARLINK_SYMBOL_COMMENT("Result of an operation that does not install or remove components."),
    &vl_type_Done,
    SD_VARLINK_SYMBOL_COMMENT("A repository that could be added as a remote."),
    &vl_type_RemoteInfo,
    SD_VARLINK_SYMBOL_COMMENT("A general flag that changes the behavior of operations."),
    &vl_type_TransactionFlag,

    SD_VARLINK_SYMBOL_COMMENT("Install the components of a bundle, which is passed as a file descriptor. "
                              "Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_InstallFile,
    SD_VARLINK_SYMBOL_COMMENT("Install a component from the remotes, with the bases it needs that are not installed. "
                              "Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_InstallComponent,
    SD_VARLINK_SYMBOL_COMMENT("Remove a component, or one version of it. "
                              "Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_RemoveComponent,
    SD_VARLINK_SYMBOL_COMMENT("Remove unused data: dependencies that no installed component builds on, images that "
                              "no component links to, and files left behind by interrupted installations. "
                              "Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_Cleanup,
    SD_VARLINK_SYMBOL_COMMENT("Download the metadata of remotes, after checking it, and make their catalogs "
                              "available. Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_Refresh,
    SD_VARLINK_SYMBOL_COMMENT("Update components to the newest version their origin has, with the bases that version "
                              "needs. Components that cannot be updated are listed in the warnings of the result. "
                              "Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_Update,
    SD_VARLINK_SYMBOL_COMMENT("Download the root index of a repository and the public key that signs it, and check "
                              "that the key verifies the root index. The repository is not added. Privileged."),
    &vl_method_InspectRemote,
    SD_VARLINK_SYMBOL_COMMENT("Add a repository as a remote that trusts the given key, and refresh it. A remote "
                              "that is masked is defined again. Called with the 'more' flag, reports its "
                              "progress. Privileged."),
    &vl_method_AddRemote,
    SD_VARLINK_SYMBOL_COMMENT("Remove a remote and its cached metadata. A remote that the vendor ships is masked. "
                              "Components that were installed from the remote stay installed, but are no longer "
                              "updated. Privileged."),
    &vl_method_RemoveRemote,
    SD_VARLINK_SYMBOL_COMMENT("Mark a version of a component as kept until it is removed explicitly (manual), "
                              "or as removed once it is not needed anymore (auto). Nothing is removed "
                              "right away. Privileged."),
    &vl_method_MarkComponent,

    SD_VARLINK_SYMBOL_COMMENT("The operation failed."),
    &vl_error_Failed,
    SD_VARLINK_SYMBOL_COMMENT("The service does not implement the requested operation."),
    &vl_error_NotImplemented,
    SD_VARLINK_SYMBOL_COMMENT("The component, or a different resource that the operation needs, was not found."),
    &vl_error_NotFound,
    SD_VARLINK_SYMBOL_COMMENT("The file is not a Manifold bundle."),
    &vl_error_InvalidBundle,
    SD_VARLINK_SYMBOL_COMMENT("The component architecture and that of the current system are incompatible."),
    &vl_error_WrongArchitecture,
    SD_VARLINK_SYMBOL_COMMENT("The contents of the bundle do not match the sizes and hashes in its index and manifests."),
    &vl_error_VerificationFailed,
    SD_VARLINK_SYMBOL_COMMENT("The base a component builds on is not installed."),
    &vl_error_MissingBase,
    SD_VARLINK_SYMBOL_COMMENT("This version of the component is installed already."),
    &vl_error_AlreadyInstalled,
    SD_VARLINK_SYMBOL_COMMENT("An installed component builds on this component."),
    &vl_error_ComponentInUse,
    SD_VARLINK_SYMBOL_COMMENT("The manifest of a component is not valid, or describes a component that can not be installed."),
    &vl_error_InvalidManifest,
    SD_VARLINK_SYMBOL_COMMENT("The component was revoked and must not be used anymore."),
    &vl_error_Revoked,
    SD_VARLINK_SYMBOL_COMMENT("Another kind of component is installed with the identifier of this component."),
    &vl_error_IdConflict,
    SD_VARLINK_SYMBOL_COMMENT("A file on the host is in the way of a file of the component."),
    &vl_error_FileConflict,
    SD_VARLINK_SYMBOL_COMMENT("The data of a repository is not valid."),
    &vl_error_InvalidRepository,
    SD_VARLINK_SYMBOL_COMMENT("The metadata of a repository has expired, so no component is installed from the repository."),
    &vl_error_Expired,
    SD_VARLINK_SYMBOL_COMMENT("A file could not be downloaded."),
    &vl_error_DownloadFailed,
    SD_VARLINK_SYMBOL_COMMENT("The request is ambiguous, e.g. several origins have the "
                              "component. The message names them."),
    &vl_error_Ambiguous,
    SD_VARLINK_SYMBOL_COMMENT("A remote with the ID of the repository is configured already."),
    &vl_error_RemoteExists);

/* clang-format on */

/* the name of the Varlink error that each #MfError is sent as */
static const struct {
    MfError code;
    const char *error_id;
} VARLINK_ERRORS[] = {
    {MF_ERROR_FAILED,              MF_INTERFACE ".Failed"            },
    {MF_ERROR_NOT_IMPLEMENTED,     MF_INTERFACE ".NotImplemented"    },
    {MF_ERROR_NOT_FOUND,           MF_INTERFACE ".NotFound"          },
    {MF_ERROR_PERMISSION_DENIED,   SD_VARLINK_ERROR_PERMISSION_DENIED},
    {MF_ERROR_INVALID_BUNDLE,      MF_INTERFACE ".InvalidBundle"     },
    {MF_ERROR_WRONG_ARCHITECTURE,  MF_INTERFACE ".WrongArchitecture" },
    {MF_ERROR_VERIFICATION_FAILED, MF_INTERFACE ".VerificationFailed"},
    {MF_ERROR_MISSING_BASE,        MF_INTERFACE ".MissingBase"       },
    {MF_ERROR_ALREADY_INSTALLED,   MF_INTERFACE ".AlreadyInstalled"  },
    {MF_ERROR_COMPONENT_IN_USE,    MF_INTERFACE ".ComponentInUse"    },
    {MF_ERROR_INVALID_MANIFEST,    MF_INTERFACE ".InvalidManifest"   },
    {MF_ERROR_REVOKED,             MF_INTERFACE ".Revoked"           },
    {MF_ERROR_ID_CONFLICT,         MF_INTERFACE ".IdConflict"        },
    {MF_ERROR_FILE_CONFLICT,       MF_INTERFACE ".FileConflict"      },
    {MF_ERROR_INVALID_REPOSITORY,  MF_INTERFACE ".InvalidRepository" },
    {MF_ERROR_EXPIRED,             MF_INTERFACE ".Expired"           },
    {MF_ERROR_DOWNLOAD_FAILED,     MF_INTERFACE ".DownloadFailed"    },
    {MF_ERROR_AMBIGUOUS,           MF_INTERFACE ".Ambiguous"         },
    {MF_ERROR_REMOTE_EXISTS,       MF_INTERFACE ".RemoteExists"      },
};

/**
 * mf_error_to_varlink_id:
 * @code: an #MfError
 *
 * Get the name of the Varlink error that @code is sent as.
 *
 * Returns: the name of the Varlink error
 */
const char *
mf_error_to_varlink_id(MfError code)
{
    for (size_t i = 0; i < G_N_ELEMENTS(VARLINK_ERRORS); i++) {
        if (VARLINK_ERRORS[i].code == code)
            return VARLINK_ERRORS[i].error_id;
    }

    return MF_INTERFACE ".Failed";
}

/**
 * mf_error_from_varlink_id:
 * @error_id: the name of a Varlink error
 * @ret_code: (out): the #MfError that @error_id stands for
 *
 * Find the #MfError that a Varlink error stands for.
 *
 * Returns: %TRUE if @error_id is an error of the service
 */
gboolean
mf_error_from_varlink_id(const char *error_id, MfError *ret_code)
{
    for (size_t i = 0; i < G_N_ELEMENTS(VARLINK_ERRORS); i++) {
        if (g_strcmp0(VARLINK_ERRORS[i].error_id, error_id) == 0) {
            *ret_code = VARLINK_ERRORS[i].code;
            return TRUE;
        }
    }

    return FALSE;
}
