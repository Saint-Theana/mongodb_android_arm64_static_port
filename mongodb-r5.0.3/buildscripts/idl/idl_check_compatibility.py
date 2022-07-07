# Copyright (C) 2021-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
# pylint: disable=too-many-lines
"""Checks compatibility of old and new IDL files.

In order to support user-selectable API versions for the server, server commands are now
defined using IDL files. This script checks that old and new commands are compatible with each
other, which allows commands to be updated without breaking the API specifications within a
specific API version.

This script accepts two directories as arguments, the "old" and the "new" IDL directory.
Before running this script, run checkout_idl_files_from_past_releases.py to find and create
directories containing the old IDL files from previous releases.
"""

import argparse
import os
import sys
from dataclasses import dataclass
from enum import Enum
from typing import Dict, List, Set, Optional, Tuple, Union

from idl import parser, syntax, errors, common
from idl.compiler import CompilerImportResolver
from idl_compatibility_errors import IDLCompatibilityContext, IDLCompatibilityErrorCollection

ALLOW_ANY_TYPE_LIST: List[str] = [
    # This list if only used in unit-tests.
    "commandAllowedAnyTypes",
    "commandAllowedAnyTypes-param-anyTypeParam",
    "commandAllowedAnyTypes-reply-anyTypeField",
    "oldTypeBsonAnyAllowList",
    "newTypeBsonAnyAllowList",
    "oldReplyFieldTypeBsonAnyAllowList-reply-oldBsonSerializationTypeAnyReplyField",
    "newReplyFieldTypeBsonAnyAllowList-reply-newBsonSerializationTypeAnyReplyField",
    "oldParamTypeBsonAnyAllowList-param-bsonTypeAnyParam",
    "newParamTypeBsonAnyAllowList-param-bsonTypeAnyParam",
    "commandAllowedAnyTypesWithVariant-reply-anyTypeField",
    "replyFieldTypeBsonAnyWithVariant-reply-bsonSerializationTypeAnyStructField",
    "replyFieldTypeBsonAnyWithVariantWithArray-reply-bsonSerializationTypeAnyStructField",
    "parameterFieldTypeBsonAnyWithVariant-param-bsonSerializationTypeAnyStructField",
    "parameterFieldTypeBsonAnyWithVariantWithArray-param-bsonSerializationTypeAnyStructField",
    "commandTypeBsonAnyWithVariant",
    "commandTypeBsonAnyWithVariantWithArray",
    "replyFieldCppTypeNotEqual-reply-cppTypeNotEqualReplyField",
    "commandCppTypeNotEqual",
    "commandParameterCppTypeNotEqual-param-cppTypeNotEqualParam",
    "replyFieldSerializerNotEqual-reply-serializerNotEqualReplyField",
    "commandSerializerNotEqual",
    "commandParameterSerializerNotEqual-param-serializerNotEqualParam",
    "replyFieldDeserializerNotEqual-reply-deserializerNotEqualReplyField",
    "commandDeserializerNotEqual",
    "commandParameterDeserializerNotEqual-param-deserializerNotEqualParam",
    "newlyAddedReplyFieldTypeBsonAnyAllowed-reply-newlyAddedBsonSerializationTypeAnyReplyField",
    "replyFieldTypeBsonAnyWithVariantUnstable-reply-bsonSerializationTypeWithVariantAnyUnstableReplyField",
    "newlyAddedParamBsonAnyAllowList-param-newlyAddedBsonAnyAllowListParam",
    "newlyAddedTypeFieldBsonAnyAllowList",
    "parameterFieldTypeBsonAnyWithVariantUnstable-param-bsonSerializationTypeAnyStructField",
    "commandTypeBsonAnyWithVariantUnstable",
    "commandParameterCppTypeNotEqualUnstable-param-cppTypeNotEqualParam",
    "replyFieldCppTypeNotEqualUnstable-reply-cppTypeNotEqualReplyUnstableField",
    "commandCppTypeNotEqualUnstable",
    "commandParameterSerializerNotEqualUnstable-param-serializerNotEqualParam",
    "replyFieldSerializerNotEqualUnstable-reply-serializerNotEqualReplyUnstableField",
    "commandSerializerNotEqualUnstable",
    "commandParameterDeserializerNotEqualUnstable-param-deserializerNotEqualParam",
    "replyFieldDeserializerNotEqualUnstable-reply-deserializerNotEqualReplyUnstableField",
    "commandDeserializerNotEqualUnstable",
    'create-param-backwards',

    # TODO (SERVER-54923): Decide what to do with commands: (saslStart, saslContinue).
    'saslStart-param-payload',
    'explain-param-use44SortKeys',
    'explain-param-useNewUpsert',
    'saslStart-param-payload',
    'saslStart-reply-payload',
    'saslContinue-param-payload',
    'saslContinue-reply-payload',

    # These commands (aggregate, find, update, delete, findAndModify, explain) might contain some
    # fields with type `any`. Currently, it's not possible to avoid the `any` type in those cases.
    # Instead, here are the preventive measures in-place to catch unintentional breaking changes:
    # 1- Added comments on top of custom serializers/deserializers (related to these fields) to
    #    let the future developers know that their modifications to these methods might lead to
    #    a breaking change in the API.
    # 2- Added proper unit-tests to catch accidental changes to the custom serializers/deserializers
    #    by over-fitting on the current implementation of these custom serializers/deserializers.
    # 3- Added further checks to the current script (idl_check_compatibility.py) to check for
    #    changing a custom serializer/deserializer and considering it as a potential breaking
    #    change.
    'aggregate-param-pipeline',
    'aggregate-param-explain',
    'aggregate-param-allowDiskUse',
    'aggregate-param-cursor',
    'aggregate-param-hint',
    'aggregate-param-needsMerge',
    'aggregate-param-fromMongos',
    'aggregate-param-$_requestReshardingResumeToken',
    'aggregate-param-isMapReduceCommand',
    'find-param-filter',
    'find-param-projection',
    'find-param-sort',
    'find-param-hint',
    'find-param-collation',
    'find-param-singleBatch',
    'find-param-allowDiskUse',
    'find-param-min',
    'find-param-max',
    'find-param-returnKey',
    'find-param-showRecordId',
    'find-param-$queryOptions',
    'find-param-tailable',
    'find-param-oplogReplay',
    'find-param-noCursorTimeout',
    'find-param-awaitData',
    'find-param-allowPartialResults',
    'find-param-readOnce',
    'find-param-allowSpeculativeMajorityRead',
    'find-param-$_requestResumeToken',
    'find-param-$_resumeAfter',
    'find-param-maxTimeMS',
    'update-param-u',
    'update-param-hint',
    'update-param-upsertSupplied',
    'update-reply-_id',
    'delete-param-limit',
    'delete-param-hint',
    'findAndModify-param-hint',
    'findAndModify-param-update',
    'findAndModify-reply-upserted',
    'explain-param-collation',
    'explain-param-use44SortKeys',
    'explain-param-useNewUpsert'
]

SKIPPED_FILES = ["unittest.idl"]


@dataclass
class FieldCompatibility:
    """Information about a Field to check compatibility."""

    field_type: syntax.Type
    idl_file: syntax.IDLParsedSpec
    idl_file_path: str
    unstable: bool


@dataclass
class FieldCompatibilityPair:
    """Information about an old and new Field pair to check compatibility."""

    old: FieldCompatibility
    new: FieldCompatibility
    cmd_name: str
    field_name: str


class ArrayTypeCheckResult(Enum):
    """Enumeration representing different return values of check_array_type."""

    INVALID = 0
    TRUE = 1
    FALSE = 2


def get_new_commands(
        ctxt: IDLCompatibilityContext, new_idl_dir: str, import_directories: List[str]
) -> Tuple[Dict[str, syntax.Command], Dict[str, syntax.IDLParsedSpec], Dict[str, str]]:
    """Get new IDL commands and check validity."""
    new_commands: Dict[str, syntax.Command] = dict()
    new_command_file: Dict[str, syntax.IDLParsedSpec] = dict()
    new_command_file_path: Dict[str, str] = dict()

    for dirpath, _, filenames in os.walk(new_idl_dir):
        for new_filename in filenames:
            if not new_filename.endswith('.idl') or new_filename in SKIPPED_FILES:
                continue

            new_idl_file_path = os.path.join(dirpath, new_filename)
            with open(new_idl_file_path) as new_file:
                new_idl_file = parser.parse(
                    new_file, new_idl_file_path,
                    CompilerImportResolver(import_directories + [new_idl_dir]))
                if new_idl_file.errors:
                    new_idl_file.errors.dump_errors()
                    raise ValueError(f"Cannot parse {new_idl_file_path}")

                for new_cmd in new_idl_file.spec.symbols.commands:
                    # Ignore imported commands as they will be processed in their own file.
                    if new_cmd.api_version == "" or new_cmd.imported:
                        continue

                    if new_cmd.api_version != "1":
                        # We're not ready to handle future API versions yet.
                        ctxt.add_command_invalid_api_version_error(
                            new_cmd.command_name, new_cmd.api_version, new_idl_file_path)
                        continue

                    if new_cmd.command_name in new_commands:
                        ctxt.add_duplicate_command_name_error(new_cmd.command_name, new_idl_dir,
                                                              new_idl_file_path)
                        continue
                    new_commands[new_cmd.command_name] = new_cmd

                    new_command_file[new_cmd.command_name] = new_idl_file
                    new_command_file_path[new_cmd.command_name] = new_idl_file_path

    return new_commands, new_command_file, new_command_file_path


def get_field_type(field: Union[syntax.Field, syntax.Command], idl_file: syntax.IDLParsedSpec,
                   idl_file_path: str) -> Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]]:
    """Resolve and get field type of a field from the IDL file."""
    parser_ctxt = errors.ParserContext(idl_file_path, errors.ParserErrorCollection())
    field_type = idl_file.spec.symbols.resolve_field_type(parser_ctxt, field, field.name,
                                                          field.type)
    if parser_ctxt.errors.has_errors():
        parser_ctxt.errors.dump_errors()
    return field_type


def check_subset(ctxt: IDLCompatibilityContext, cmd_name: str, field_name: str, type_name: str,
                 sub_list: List[Union[str, syntax.EnumValue]],
                 super_list: List[Union[str, syntax.EnumValue]], file_path: str):
    # pylint: disable=too-many-arguments
    """Check if sub_list is a subset of the super_list and log an error if not."""
    if not set(sub_list).issubset(super_list):
        ctxt.add_reply_field_not_subset_error(cmd_name, field_name, type_name, file_path)


def check_superset(ctxt: IDLCompatibilityContext, cmd_name: str, type_name: str,
                   super_list: List[Union[str, syntax.EnumValue]],
                   sub_list: List[Union[str, syntax.EnumValue]], file_path: str,
                   param_name: Optional[str], is_command_parameter: bool):
    # pylint: disable=too-many-arguments
    """Check if super_list is a superset of the sub_list and log an error if not."""
    if not set(super_list).issuperset(sub_list):
        ctxt.add_command_or_param_type_not_superset_error(cmd_name, type_name, file_path,
                                                          param_name, is_command_parameter)


def check_reply_field_type_recursive(ctxt: IDLCompatibilityContext,
                                     field_pair: FieldCompatibilityPair) -> None:
    # pylint: disable=too-many-branches
    """Check compatibility between old and new reply field type if old field type is a syntax.Type instance."""
    old_field = field_pair.old
    new_field = field_pair.new
    old_field_type = old_field.field_type
    new_field_type = new_field.field_type
    cmd_name = field_pair.cmd_name
    field_name = field_pair.field_name

    # If the old field is unstable, we only add errors related to the use of 'any' as the
    # bson_serialization_type. For all other errors, we check that the old field is stable
    # before adding an error.
    if not isinstance(new_field_type, syntax.Type):
        if not old_field.unstable:
            ctxt.add_new_reply_field_type_enum_or_struct_error(
                cmd_name, field_name, new_field_type.name, old_field_type.name,
                new_field.idl_file_path)
        return

    # If bson_serialization_type switches from 'any' to non-any type.
    if "any" in old_field_type.bson_serialization_type and "any" not in new_field_type.bson_serialization_type:
        ctxt.add_old_reply_field_bson_any_error(cmd_name, field_name, old_field_type.name,
                                                old_field.idl_file_path)
        return

    # If bson_serialization_type switches from non-any to 'any' type.
    if "any" not in old_field_type.bson_serialization_type and "any" in new_field_type.bson_serialization_type:
        ctxt.add_new_reply_field_bson_any_error(cmd_name, field_name, old_field_type.name,
                                                new_field.idl_file_path)
        return

    allow_name: str = cmd_name + "-reply-" + field_name

    if "any" in old_field_type.bson_serialization_type:
        # If 'any' is not explicitly allowed as the bson_serialization_type.
        if allow_name not in ALLOW_ANY_TYPE_LIST:
            ctxt.add_reply_field_bson_any_not_allowed_error(
                cmd_name, field_name, old_field_type.name, old_field.idl_file_path)
            return

        # If cpp_type is changed, it's a potential breaking change.
        if old_field_type.cpp_type != new_field_type.cpp_type:
            ctxt.add_reply_field_cpp_type_not_equal_error(cmd_name, field_name, new_field_type.name,
                                                          new_field.idl_file_path)

        # If serializer is changed, it's a potential breaking change.
        if (not old_field.unstable) and old_field_type.serializer != new_field_type.serializer:
            ctxt.add_reply_field_serializer_not_equal_error(
                cmd_name, field_name, new_field_type.name, new_field.idl_file_path)

        # If deserializer is changed, it's a potential breaking change.
        if (not old_field.unstable) and old_field_type.deserializer != new_field_type.deserializer:
            ctxt.add_reply_field_deserializer_not_equal_error(
                cmd_name, field_name, new_field_type.name, new_field.idl_file_path)

    if isinstance(old_field_type, syntax.VariantType):
        # If the new type is not variant just check the single type.
        new_variant_types = new_field_type.variant_types if isinstance(
            new_field_type, syntax.VariantType) else [new_field_type]
        old_variant_types = old_field_type.variant_types

        # Check that new variant types are a subset of old variant types.
        for new_variant_type in new_variant_types:
            for old_variant_type in old_variant_types:
                if old_variant_type.name == new_variant_type.name:
                    # Check that the old and new version of each variant type is also compatible.
                    old = FieldCompatibility(old_variant_type, old_field.idl_file,
                                             old_field.idl_file_path, old_field.unstable)
                    new = FieldCompatibility(new_variant_type, new_field.idl_file,
                                             new_field.idl_file_path, new_field.unstable)
                    check_reply_field_type(ctxt,
                                           FieldCompatibilityPair(old, new, cmd_name, field_name))
                    break

            else:
                # new_variant_type was not found in old_variant_types.
                if not old_field.unstable:
                    ctxt.add_new_reply_field_variant_type_not_subset_error(
                        cmd_name, field_name, new_variant_type.name, new_field.idl_file_path)

        # If new type is variant and has a struct as a variant type, compare old and new variant_struct_type.
        # Since enums can't be part of variant types, we don't explicitly check for enums.
        if isinstance(new_field_type,
                      syntax.VariantType) and new_field_type.variant_struct_type is not None:
            if old_field_type.variant_struct_type is None and not old_field.unstable:
                ctxt.add_new_reply_field_variant_type_not_subset_error(
                    cmd_name, field_name, new_field_type.variant_struct_type.name,
                    new_field.idl_file_path)
            else:
                check_reply_fields(ctxt, old_field_type.variant_struct_type,
                                   new_field_type.variant_struct_type, cmd_name, old_field.idl_file,
                                   new_field.idl_file, old_field.idl_file_path,
                                   new_field.idl_file_path)

    elif not old_field.unstable:
        if isinstance(new_field_type, syntax.VariantType):
            ctxt.add_new_reply_field_variant_type_error(cmd_name, field_name, old_field_type.name,
                                                        new_field.idl_file_path)
        else:
            check_subset(ctxt, cmd_name, field_name, new_field_type.name,
                         new_field_type.bson_serialization_type,
                         old_field_type.bson_serialization_type, new_field.idl_file_path)


def check_reply_field_type(ctxt: IDLCompatibilityContext, field_pair: FieldCompatibilityPair):
    """Check compatibility between old and new reply field type."""
    # pylint: disable=too-many-branches
    old_field = field_pair.old
    new_field = field_pair.new
    array_check = check_array_type(ctxt, "reply_field", old_field.field_type, new_field.field_type,
                                   field_pair.cmd_name, 'type', old_field.idl_file_path,
                                   new_field.idl_file_path, old_field.unstable)
    if array_check == ArrayTypeCheckResult.INVALID:
        return

    if array_check == ArrayTypeCheckResult.TRUE:
        old_field.field_type = old_field.field_type.element_type
        new_field.field_type = new_field.field_type.element_type

    old_field_type = old_field.field_type
    new_field_type = new_field.field_type
    cmd_name = field_pair.cmd_name
    field_name = field_pair.field_name
    if old_field_type is None:
        ctxt.add_reply_field_type_invalid_error(cmd_name, field_name, old_field.idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)
    if new_field_type is None:
        ctxt.add_reply_field_type_invalid_error(cmd_name, field_name, new_field.idl_file_path)
        ctxt.errors.dump_errors()
        sys.exit(1)

    if isinstance(old_field_type, syntax.Type):
        check_reply_field_type_recursive(ctxt, field_pair)

    elif isinstance(old_field_type, syntax.Enum) and not old_field.unstable:
        if isinstance(new_field_type, syntax.Enum):
            check_subset(ctxt, cmd_name, field_name, new_field_type.name, new_field_type.values,
                         old_field_type.values, new_field.idl_file_path)
        else:
            ctxt.add_new_reply_field_type_not_enum_error(cmd_name, field_name, new_field_type.name,
                                                         old_field_type.name,
                                                         new_field.idl_file_path)
    elif isinstance(old_field_type, syntax.Struct):
        if isinstance(new_field_type, syntax.Struct):
            check_reply_fields(ctxt, old_field_type, new_field_type, cmd_name, old_field.idl_file,
                               new_field.idl_file, old_field.idl_file_path, new_field.idl_file_path)
        else:
            if not old_field.unstable:
                ctxt.add_new_reply_field_type_not_struct_error(
                    cmd_name, field_name, new_field_type.name, old_field_type.name,
                    new_field.idl_file_path)


def check_array_type(ctxt: IDLCompatibilityContext, symbol: str,
                     old_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]],
                     new_type: Optional[Union[syntax.Enum, syntax.Struct, syntax.Type]],
                     cmd_name: str, symbol_name: str, old_idl_file_path: str,
                     new_idl_file_path: str, old_field_unstable: bool) -> ArrayTypeCheckResult:
    """
    Check compatibility between old and new ArrayTypes.

    :returns:
        -  ArrayTypeCheckResult.TRUE : when the old type and new type are of array type.
        -  ArrayTypeCheckResult.FALSE : when the old type and new type aren't of array type.
        -  ArrayTypeCheckResult.INVALID : when one of the types is not of array type while the other one is.
    """
    # pylint: disable=too-many-arguments,too-many-branches
    old_is_array = isinstance(old_type, syntax.ArrayType)
    new_is_array = isinstance(new_type, syntax.ArrayType)
    if not old_is_array and not new_is_array:
        return ArrayTypeCheckResult.FALSE

    if (not old_is_array or not new_is_array) and not old_field_unstable:
        ctxt.add_type_not_array_error(symbol, cmd_name, symbol_name, new_type.name, old_type.name,
                                      new_idl_file_path if old_is_array else old_idl_file_path)
        return ArrayTypeCheckResult.INVALID

    return ArrayTypeCheckResult.TRUE


def check_reply_field(ctxt: IDLCompatibilityContext, old_field: syntax.Field,
                      new_field: syntax.Field, cmd_name: str, old_idl_file: syntax.IDLParsedSpec,
                      new_idl_file: syntax.IDLParsedSpec, old_idl_file_path: str,
                      new_idl_file_path: str):
    """Check compatibility between old and new reply field."""
    # pylint: disable=too-many-arguments
    if not old_field.unstable:
        if new_field.unstable:
            ctxt.add_new_reply_field_unstable_error(cmd_name, new_field.name, new_idl_file_path)
        if new_field.optional and not old_field.optional:
            ctxt.add_new_reply_field_optional_error(cmd_name, new_field.name, new_idl_file_path)

        if new_field.validator:
            if old_field.validator:
                if new_field.validator != old_field.validator:
                    ctxt.add_reply_field_validators_not_equal_error(cmd_name, new_field.name,
                                                                    new_idl_file_path)
            else:
                ctxt.add_reply_field_contains_validator_error(cmd_name, new_field.name,
                                                              new_idl_file_path)

    old_field_type = get_field_type(old_field, old_idl_file, old_idl_file_path)
    new_field_type = get_field_type(new_field, new_idl_file, new_idl_file_path)

    old_field_compatibility = FieldCompatibility(old_field_type, old_idl_file, old_idl_file_path,
                                                 old_field.unstable)
    new_field_compatibility = FieldCompatibility(new_field_type, new_idl_file, new_idl_file_path,
                                                 new_field.unstable)
    field_pair = FieldCompatibilityPair(old_field_compatibility, new_field_compatibility, cmd_name,
                                        old_field.name)

    check_reply_field_type(ctxt, field_pair)


def check_reply_fields(ctxt: IDLCompatibilityContext, old_reply: syntax.Struct,
                       new_reply: syntax.Struct, cmd_name: str, old_idl_file: syntax.IDLParsedSpec,
                       new_idl_file: syntax.IDLParsedSpec, old_idl_file_path: str,
                       new_idl_file_path: str):
    """Check compatibility between old and new reply fields."""
    # pylint: disable=too-many-arguments
    for old_field in old_reply.fields or []:
        new_field_exists = False
        for new_field in new_reply.fields or []:
            if new_field.name == old_field.name:
                new_field_exists = True
                check_reply_field(ctxt, old_field, new_field, cmd_name, old_idl_file, new_idl_file,
                                  old_idl_file_path, new_idl_file_path)

                break

        if not new_field_exists and not old_field.unstable:
            ctxt.add_new_reply_field_missing_error(cmd_name, old_field.name, old_idl_file_path)

    # Check that newly added fields do not have an unallowed use of 'any' as the bson_serialization_type.
    for new_field in new_reply.fields or []:
        newly_added = True
        for old_field in old_reply.fields or []:
            if new_field.name == old_field.name:
                newly_added = False

        if newly_added:
            allow_name: str = cmd_name + "-reply-" + new_field.name

            new_field_type = get_field_type(new_field, new_idl_file, new_idl_file_path)
            if isinstance(new_field_type,
                          syntax.Type) and "any" in new_field_type.bson_serialization_type:
                # If 'any' is not explicitly allowed as the bson_serialization_type.
                if allow_name not in ALLOW_ANY_TYPE_LIST:
                    ctxt.add_reply_field_bson_any_not_allowed_error(
                        cmd_name, new_field.name, new_field_type.name, new_idl_file_path)


def check_param_or_command_type_recursive(ctxt: IDLCompatibilityContext,
                                          field_pair: FieldCompatibilityPair,
                                          is_command_parameter: bool):
    # pylint: disable=too-many-branches,too-many-locals
    """
    Check compatibility between old and new command or param type recursively.

    If the old type is a syntax.Type instance, check the compatibility between the old and new
    command type or parameter type recursively.
    """
    old_field = field_pair.old
    new_field = field_pair.new
    old_type = old_field.field_type
    new_type = new_field.field_type
    cmd_name = field_pair.cmd_name
    param_name = field_pair.field_name

    # If the old field is unstable, we only add errors related to the use of 'any' as the
    # bson_serialization_type. For all other errors, we check that the old field is stable
    # before adding an error.

    if not isinstance(new_type, syntax.Type):
        if not old_field.unstable:
            ctxt.add_new_command_or_param_type_enum_or_struct_error(
                cmd_name, new_type.name, old_type.name, new_field.idl_file_path, param_name,
                is_command_parameter)
        return

    if old_type.bson_serialization_type is None or new_type.bson_serialization_type is None:
        print("here")
    # If bson_serialization_type switches from 'any' to non-any type.
    if "any" in old_type.bson_serialization_type and "any" not in new_type.bson_serialization_type:
        ctxt.add_old_command_or_param_type_bson_any_error(
            cmd_name, old_type.name, old_field.idl_file_path, param_name, is_command_parameter)
        return

    # If bson_serialization_type switches from non-any to 'any' type.
    if "any" not in old_type.bson_serialization_type and "any" in new_type.bson_serialization_type:
        ctxt.add_new_command_or_param_type_bson_any_error(
            cmd_name, new_type.name, new_field.idl_file_path, param_name, is_command_parameter)
        return

    allow_name: str = cmd_name + "-param-" + param_name if is_command_parameter else cmd_name

    if "any" in old_type.bson_serialization_type:
        # If 'any' is not explicitly allowed as the bson_serialization_type.
        if allow_name not in ALLOW_ANY_TYPE_LIST:
            ctxt.add_command_or_param_type_bson_any_not_allowed_error(
                cmd_name, old_type.name, old_field.idl_file_path, param_name, is_command_parameter)
            return

        # If cpp_type is changed, it's a potential breaking change.
        if old_type.cpp_type != new_type.cpp_type:
            ctxt.add_command_or_param_cpp_type_not_equal_error(
                cmd_name, new_type.name, new_field.idl_file_path, param_name, is_command_parameter)

        # If serializer is changed, it's a potential breaking change.
        if (not old_field.unstable) and old_type.serializer != new_type.serializer:
            ctxt.add_command_or_param_serializer_not_equal_error(
                cmd_name, new_type.name, new_field.idl_file_path, param_name, is_command_parameter)

        # If deserializer is changed, it's a potential breaking change.
        if (not old_field.unstable) and old_type.deserializer != new_type.deserializer:
            ctxt.add_command_or_param_deserializer_not_equal_error(
                cmd_name, new_type.name, new_field.idl_file_path, param_name, is_command_parameter)

    if isinstance(old_type, syntax.VariantType):
        if not isinstance(new_type, syntax.VariantType):
            if not old_field.unstable:
                ctxt.add_new_command_or_param_type_not_variant_type_error(
                    cmd_name, new_type.name, new_field.idl_file_path, param_name,
                    is_command_parameter)
        else:
            new_variant_types = new_type.variant_types
            old_variant_types = old_type.variant_types

            # Check that new variant types are a superset of old variant types.
            for old_variant_type in old_variant_types:
                for new_variant_type in new_variant_types:
                    if old_variant_type.name == new_variant_type.name:
                        # Check that the old and new version of each variant type is also compatible.
                        old = FieldCompatibility(old_variant_type, old_field.idl_file,
                                                 old_field.idl_file_path, old_field.unstable)
                        new = FieldCompatibility(new_variant_type, new_field.idl_file,
                                                 new_field.idl_file_path, new_field.unstable)
                        check_param_or_command_type(
                            ctxt, FieldCompatibilityPair(old, new, cmd_name, param_name),
                            is_command_parameter)
                        break
                else:
                    if not old_field.unstable:
                        # old_variant_type was not found in new_variant_types.
                        ctxt.add_new_command_or_param_variant_type_not_superset_error(
                            cmd_name, old_variant_type.name, new_field.idl_file_path, param_name,
                            is_command_parameter)

            # If old and new types both have a struct as a variant type, compare old and new variant_struct_type.
            # Since enums can't be part of variant types, we don't explicitly check for enums.
            if old_type.variant_struct_type is not None:
                if new_type.variant_struct_type is not None:
                    check_command_params_or_type_struct_fields(
                        ctxt, old_type.variant_struct_type, new_type.variant_struct_type, cmd_name,
                        old_field.idl_file, new_field.idl_file, old_field.idl_file_path,
                        new_field.idl_file_path, is_command_parameter)

                # If old type has a variant struct type and new type does not have a variant struct type.
                elif not old_field.unstable:
                    ctxt.add_new_command_or_param_variant_type_not_superset_error(
                        cmd_name, old_type.variant_struct_type.name, new_field.idl_file_path,
                        param_name, is_command_parameter)

    elif not old_field.unstable:
        check_superset(ctxt, cmd_name, new_type.name, new_type.bson_serialization_type,
                       old_type.bson_serialization_type, new_field.idl_file_path, param_name,
                       is_command_parameter)


def check_param_or_command_type(ctxt: IDLCompatibilityContext, field_pair: FieldCompatibilityPair,
                                is_command_parameter: bool):
    """Check compatibility between old and new command parameter type or command type."""
    # pylint: disable=too-many-branches
    old_field = field_pair.old
    new_field = field_pair.new
    array_check = check_array_type(
        ctxt, "command_parameter" if is_command_parameter else "command_namespace",
        old_field.field_type, new_field.field_type, field_pair.cmd_name,
        field_pair.field_name if is_command_parameter else "type", old_field.idl_file_path,
        new_field.idl_file_path, old_field.unstable)
    if array_check == ArrayTypeCheckResult.INVALID:
        return

    if array_check == ArrayTypeCheckResult.TRUE:
        old_field.field_type = old_field.field_type.element_type
        new_field.field_type = new_field.field_type.element_type

    old_type = old_field.field_type
    new_type = new_field.field_type
    if old_type is None:
        ctxt.add_command_or_param_type_invalid_error(field_pair.cmd_name, old_field.idl_file_path,
                                                     field_pair.field_name, is_command_parameter)
        ctxt.errors.dump_errors()
        sys.exit(1)
    if new_type is None:
        ctxt.add_command_or_param_type_invalid_error(field_pair.cmd_name, new_field.idl_file_path,
                                                     field_pair.field_name, is_command_parameter)
        ctxt.errors.dump_errors()
        sys.exit(1)

    if isinstance(old_type, syntax.Type):
        check_param_or_command_type_recursive(ctxt, field_pair, is_command_parameter)

    # Only add type errors if the old field is stable.
    elif isinstance(old_type, syntax.Enum) and not old_field.unstable:
        if isinstance(new_type, syntax.Enum):
            check_superset(ctxt, field_pair.cmd_name, new_type.name, new_type.values,
                           old_type.values, new_field.idl_file_path, field_pair.field_name,
                           is_command_parameter)
        else:
            ctxt.add_new_command_or_param_type_not_enum_error(
                field_pair.cmd_name, new_type.name, old_type.name, new_field.idl_file_path,
                field_pair.field_name, is_command_parameter)

    elif isinstance(old_type, syntax.Struct):
        if isinstance(new_type, syntax.Struct):
            check_command_params_or_type_struct_fields(
                ctxt, old_type, new_type, field_pair.cmd_name, old_field.idl_file,
                new_field.idl_file, old_field.idl_file_path, new_field.idl_file_path,
                is_command_parameter)
        else:
            if not old_field.unstable:
                ctxt.add_new_command_or_param_type_not_struct_error(
                    field_pair.cmd_name, new_type.name, old_type.name, new_field.idl_file_path,
                    field_pair.field_name, is_command_parameter)


def check_param_or_type_validator(ctxt: IDLCompatibilityContext, old_field: syntax.Field,
                                  new_field: syntax.Field, cmd_name: str, new_idl_file_path: str,
                                  type_name: Optional[str], is_command_parameter: bool):
    """
    Check compatibility between old and new validators.

    Check compatibility between old and new validators in command parameter type and command type
    struct fields.
    """
    # pylint: disable=too-many-arguments
    if new_field.validator:
        if old_field.validator:
            if new_field.validator != old_field.validator:
                ctxt.add_command_or_param_type_validators_not_equal_error(
                    cmd_name, new_field.name, new_idl_file_path, type_name, is_command_parameter)
        else:
            ctxt.add_command_or_param_type_contains_validator_error(
                cmd_name, new_field.name, new_idl_file_path, type_name, is_command_parameter)


def check_command_params_or_type_struct_fields(
        ctxt: IDLCompatibilityContext, old_struct: syntax.Struct, new_struct: syntax.Struct,
        cmd_name: str, old_idl_file: syntax.IDLParsedSpec, new_idl_file: syntax.IDLParsedSpec,
        old_idl_file_path: str, new_idl_file_path: str, is_command_parameter: bool):
    """Check compatibility between old and new parameters or command type fields."""
    # pylint: disable=too-many-arguments
    for old_field in old_struct.fields or []:
        new_field_exists = False
        for new_field in new_struct.fields or []:
            if new_field.name == old_field.name:
                new_field_exists = True
                check_command_param_or_type_struct_field(
                    ctxt, old_field, new_field, cmd_name, old_idl_file, new_idl_file,
                    old_idl_file_path, new_idl_file_path, old_struct.name, is_command_parameter)

                break

        if not new_field_exists and not old_field.unstable:
            ctxt.add_new_param_or_command_type_field_missing_error(
                cmd_name, old_field.name, old_idl_file_path, old_struct.name, is_command_parameter)

    # Check if a new field has been added to the parameters or type struct.
    # If so, it must be optional.
    for new_field in new_struct.fields or []:
        newly_added = True
        for old_field in old_struct.fields or []:
            if new_field.name == old_field.name:
                newly_added = False

        if newly_added and not new_field.optional and not new_field.unstable:
            ctxt.add_new_param_or_command_type_field_added_required_error(
                cmd_name, new_field.name, new_idl_file_path, new_struct.name, is_command_parameter)

        # Check that a new field does not have an unallowed use of 'any' as the bson_serialization_type.
        if newly_added:
            allow_name: str = cmd_name + "-param-" + new_field.name if is_command_parameter else cmd_name
            new_field_type = get_field_type(new_field, new_idl_file, new_idl_file_path)
            if isinstance(new_field_type,
                          syntax.Type) and "any" in new_field_type.bson_serialization_type:
                # If 'any' is not explicitly allowed as the bson_serialization_type.
                if allow_name not in ALLOW_ANY_TYPE_LIST:
                    ctxt.add_command_or_param_type_bson_any_not_allowed_error(
                        cmd_name, new_field_type.name, old_idl_file_path, new_field.name,
                        is_command_parameter)


def check_command_param_or_type_struct_field(
        ctxt: IDLCompatibilityContext, old_field: syntax.Field, new_field: syntax.Field,
        cmd_name: str, old_idl_file: syntax.IDLParsedSpec, new_idl_file: syntax.IDLParsedSpec,
        old_idl_file_path: str, new_idl_file_path: str, type_name: Optional[str],
        is_command_parameter: bool):
    """Check compatibility between the old and new command parameter or command type struct field."""
    # pylint: disable=too-many-arguments
    if not old_field.unstable and new_field.unstable:
        ctxt.add_new_param_or_command_type_field_unstable_error(
            cmd_name, old_field.name, old_idl_file_path, type_name, is_command_parameter)
    # If old field is unstable and new field is stable, the new field should either be optional or
    # have a default value.
    if old_field.unstable and not new_field.unstable and not new_field.optional and new_field.default is None:
        ctxt.add_new_param_or_command_type_field_stable_required_no_default_error(
            cmd_name, old_field.name, old_idl_file_path, type_name, is_command_parameter)

    if old_field.optional and not new_field.optional:
        ctxt.add_new_param_or_command_type_field_required_error(
            cmd_name, old_field.name, old_idl_file_path, type_name, is_command_parameter)

    if not old_field.unstable:
        check_param_or_type_validator(ctxt, old_field, new_field, cmd_name, new_idl_file_path,
                                      type_name, is_command_parameter)

    old_field_type = get_field_type(old_field, old_idl_file, old_idl_file_path)
    new_field_type = get_field_type(new_field, new_idl_file, new_idl_file_path)

    old_field_compatibility = FieldCompatibility(old_field_type, old_idl_file, old_idl_file_path,
                                                 old_field.unstable)
    new_field_compatibility = FieldCompatibility(new_field_type, new_idl_file, new_idl_file_path,
                                                 new_field.unstable)
    field_pair = FieldCompatibilityPair(old_field_compatibility, new_field_compatibility, cmd_name,
                                        old_field.name)

    check_param_or_command_type(ctxt, field_pair, is_command_parameter)


def check_namespace(ctxt: IDLCompatibilityContext, old_cmd: syntax.Command, new_cmd: syntax.Command,
                    old_idl_file: syntax.IDLParsedSpec, new_idl_file: syntax.IDLParsedSpec,
                    old_idl_file_path: str, new_idl_file_path: str):
    """Check compatibility between old and new namespace."""
    # pylint: disable=too-many-arguments
    old_namespace = old_cmd.namespace
    new_namespace = new_cmd.namespace

    # IDL parser already checks that namespace must be one of these 4 types.
    if old_namespace == common.COMMAND_NAMESPACE_IGNORED:
        if new_namespace != common.COMMAND_NAMESPACE_IGNORED:
            ctxt.add_new_namespace_incompatible_error(old_cmd.command_name, old_namespace,
                                                      new_namespace, new_idl_file_path)
    elif old_namespace == common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB_OR_UUID:
        if new_namespace not in (common.COMMAND_NAMESPACE_IGNORED,
                                 common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB_OR_UUID):
            ctxt.add_new_namespace_incompatible_error(old_cmd.command_name, old_namespace,
                                                      new_namespace, new_idl_file_path)
    elif old_namespace == common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB:
        if new_namespace == common.COMMAND_NAMESPACE_TYPE:
            ctxt.add_new_namespace_incompatible_error(old_cmd.command_name, old_namespace,
                                                      new_namespace, new_idl_file_path)
    elif old_namespace == common.COMMAND_NAMESPACE_TYPE:
        old_type = get_field_type(old_cmd, old_idl_file, old_idl_file_path)
        if new_namespace == common.COMMAND_NAMESPACE_TYPE:
            new_type = get_field_type(new_cmd, new_idl_file, new_idl_file_path)
            old = FieldCompatibility(old_type, old_idl_file, old_idl_file_path, unstable=False)
            new = FieldCompatibility(new_type, new_idl_file, new_idl_file_path, unstable=False)

            check_param_or_command_type(ctxt,
                                        FieldCompatibilityPair(old, new, old_cmd.command_name, ""),
                                        is_command_parameter=False)

        # If old type is "namespacestring", the new namespace can be changed to any
        # of the other namespace types.
        elif old_type.name != "namespacestring":
            # Otherwise, the new namespace can only be changed to "ignored".
            if new_namespace != common.COMMAND_NAMESPACE_IGNORED:
                ctxt.add_new_namespace_incompatible_error(old_cmd.command_name, old_namespace,
                                                          new_namespace, new_idl_file_path)
    else:
        assert False, 'unrecognized namespace option'


def check_error_reply(old_basic_types_path: str, new_basic_types_path: str,
                      import_directories: List[str]) -> IDLCompatibilityErrorCollection:
    """Check IDL compatibility between old and new ErrorReply."""
    old_idl_dir = os.path.dirname(old_basic_types_path)
    new_idl_dir = os.path.dirname(new_basic_types_path)
    ctxt = IDLCompatibilityContext(old_idl_dir, new_idl_dir, IDLCompatibilityErrorCollection())
    with open(old_basic_types_path) as old_file:
        old_idl_file = parser.parse(old_file, old_basic_types_path,
                                    CompilerImportResolver(import_directories))
        if old_idl_file.errors:
            old_idl_file.errors.dump_errors()
            raise ValueError(f"Cannot parse {old_basic_types_path}")

        old_error_reply_struct = old_idl_file.spec.symbols.get_struct("ErrorReply")

        if old_error_reply_struct is None:
            ctxt.add_missing_error_reply_struct_error(old_basic_types_path)
        else:
            with open(new_basic_types_path) as new_file:
                new_idl_file = parser.parse(new_file, new_basic_types_path,
                                            CompilerImportResolver(import_directories))
                if new_idl_file.errors:
                    new_idl_file.errors.dump_errors()
                    raise ValueError(f"Cannot parse {new_basic_types_path}")

                new_error_reply_struct = new_idl_file.spec.symbols.get_struct("ErrorReply")
                if new_error_reply_struct is None:
                    ctxt.add_missing_error_reply_struct_error(new_basic_types_path)
                else:
                    check_reply_fields(ctxt, old_error_reply_struct, new_error_reply_struct, "n/a",
                                       old_idl_file, new_idl_file, old_basic_types_path,
                                       new_basic_types_path)

    ctxt.errors.dump_errors()
    return ctxt.errors


def split_complex_checks(
        complex_checks: List[syntax.AccessCheck]) -> Tuple[List[str], List[syntax.Privilege]]:
    """Split a list of AccessCheck into checks and privileges."""
    checks = [x.check for x in complex_checks if x.check is not None]
    privileges = [x.privilege for x in complex_checks if x.privilege is not None]
    # Sort the list of privileges by the length of the action_type list, in decreasing order
    # so that two lists of privileges can be compared later.
    return checks, sorted(privileges, key=lambda x: len(x.action_type), reverse=True)


def check_security_access_checks(ctxt: IDLCompatibilityContext,
                                 old_access_checks: syntax.AccessChecks,
                                 new_access_checks: syntax.AccessChecks, cmd: syntax.Command,
                                 new_idl_file_path: str) -> None:
    """Check the compatibility between security access checks of the old and new command."""
    # pylint:disable=too-many-locals,too-many-branches,too-many-nested-blocks
    cmd_name = cmd.command_name
    if old_access_checks is not None and new_access_checks is not None:
        old_access_check_type = old_access_checks.get_access_check_type()
        new_access_check_type = new_access_checks.get_access_check_type()
        if old_access_check_type != new_access_check_type:
            ctxt.add_access_check_type_not_equal_error(cmd_name, old_access_check_type,
                                                       new_access_check_type, new_idl_file_path)
        else:
            old_simple_check = old_access_checks.simple
            new_simple_check = new_access_checks.simple
            if old_simple_check is not None and new_simple_check is not None:
                if old_simple_check.check != new_simple_check.check:
                    ctxt.add_check_not_equal_error(cmd_name, old_simple_check.check,
                                                   new_simple_check.check, new_idl_file_path)
                else:
                    old_privilege = old_simple_check.privilege
                    new_privilege = new_simple_check.privilege
                    if old_privilege is not None and new_privilege is not None:
                        if old_privilege.resource_pattern != new_privilege.resource_pattern:
                            ctxt.add_resource_pattern_not_equal_error(
                                cmd_name, old_privilege.resource_pattern,
                                new_privilege.resource_pattern, new_idl_file_path)
                        if not set(new_privilege.action_type).issubset(old_privilege.action_type):
                            ctxt.add_new_action_types_not_subset_error(cmd_name, new_idl_file_path)

            old_complex_checks = old_access_checks.complex
            new_complex_checks = new_access_checks.complex
            if old_complex_checks is not None and new_complex_checks is not None:
                if len(new_complex_checks) > len(old_complex_checks):
                    ctxt.add_new_additional_complex_access_check_error(cmd_name, new_idl_file_path)
                else:
                    old_checks, old_privileges = split_complex_checks(old_complex_checks)
                    new_checks, new_privileges = split_complex_checks(new_complex_checks)
                    if not set(new_checks).issubset(old_checks):
                        ctxt.add_new_complex_checks_not_subset_error(cmd_name, new_idl_file_path)

                    if len(new_privileges) > len(old_privileges):
                        ctxt.add_new_complex_privileges_not_subset_error(
                            cmd_name, new_idl_file_path)
                    else:
                        # Check that each new_privilege matches an old_privilege (the resource_pattern is
                        # equal and the action_types are a subset of the old action_types).
                        for new_privilege in new_privileges:
                            for old_privilege in old_privileges:
                                if (new_privilege.resource_pattern == old_privilege.resource_pattern
                                        and set(new_privilege.action_type).issubset(
                                            old_privilege.action_type)):
                                    old_privileges.remove(old_privilege)
                                    break
                            else:
                                ctxt.add_new_complex_privileges_not_subset_error(
                                    cmd_name, new_idl_file_path)

    elif new_access_checks is None and old_access_checks is not None:
        ctxt.add_removed_access_check_field_error(cmd_name, new_idl_file_path)
    elif old_access_checks is None and new_access_checks is not None and cmd.api_version == '1':
        ctxt.add_added_access_check_field_error(cmd_name, new_idl_file_path)


def check_compatibility(old_idl_dir: str, new_idl_dir: str,
                        import_directories: List[str]) -> IDLCompatibilityErrorCollection:
    """Check IDL compatibility between old and new IDL commands."""
    # pylint: disable=too-many-locals
    ctxt = IDLCompatibilityContext(old_idl_dir, new_idl_dir, IDLCompatibilityErrorCollection())

    new_commands, new_command_file, new_command_file_path = get_new_commands(
        ctxt, new_idl_dir, import_directories)

    # Check new commands' compatibility with old ones.
    # Note, a command can be added to V1 at any time, it's ok if a
    # new command has no corresponding old command.
    old_commands: Dict[str, syntax.Command] = dict()
    for dirpath, _, filenames in os.walk(old_idl_dir):
        for old_filename in filenames:
            if not old_filename.endswith('.idl') or old_filename in SKIPPED_FILES:
                continue

            old_idl_file_path = os.path.join(dirpath, old_filename)
            with open(old_idl_file_path) as old_file:
                old_idl_file = parser.parse(
                    old_file, old_idl_file_path,
                    CompilerImportResolver(import_directories + [old_idl_dir]))
                if old_idl_file.errors:
                    old_idl_file.errors.dump_errors()
                    raise ValueError(f"Cannot parse {old_idl_file_path}")

                for old_cmd in old_idl_file.spec.symbols.commands:
                    # Ignore imported commands as they will be processed in their own file.
                    if old_cmd.api_version == "" or old_cmd.imported:
                        continue

                    if old_cmd.api_version != "1":
                        # We're not ready to handle future API versions yet.
                        ctxt.add_command_invalid_api_version_error(
                            old_cmd.command_name, old_cmd.api_version, old_idl_file_path)
                        continue

                    if old_cmd.command_name in old_commands:
                        ctxt.add_duplicate_command_name_error(old_cmd.command_name, old_idl_dir,
                                                              old_idl_file_path)
                        continue

                    old_commands[old_cmd.command_name] = old_cmd

                    if old_cmd.command_name not in new_commands:
                        # Can't remove a command from V1
                        ctxt.add_command_removed_error(old_cmd.command_name, old_idl_file_path)
                        continue

                    new_cmd = new_commands[old_cmd.command_name]
                    new_idl_file = new_command_file[old_cmd.command_name]
                    new_idl_file_path = new_command_file_path[old_cmd.command_name]

                    if not old_cmd.strict and new_cmd.strict:
                        ctxt.add_command_strict_true_error(new_cmd.command_name, new_idl_file_path)

                    # Check compatibility of command's parameters.
                    check_command_params_or_type_struct_fields(
                        ctxt, old_cmd, new_cmd, old_cmd.command_name, old_idl_file, new_idl_file,
                        old_idl_file_path, new_idl_file_path, is_command_parameter=True)

                    check_namespace(ctxt, old_cmd, new_cmd, old_idl_file, new_idl_file,
                                    old_idl_file_path, new_idl_file_path)

                    old_reply = old_idl_file.spec.symbols.get_struct(old_cmd.reply_type)
                    new_reply = new_idl_file.spec.symbols.get_struct(new_cmd.reply_type)
                    check_reply_fields(ctxt, old_reply, new_reply, old_cmd.command_name,
                                       old_idl_file, new_idl_file, old_idl_file_path,
                                       new_idl_file_path)

                    check_security_access_checks(ctxt, old_cmd.access_check, new_cmd.access_check,
                                                 old_cmd, new_idl_file_path)

    ctxt.errors.dump_errors()
    return ctxt.errors


def get_generic_arguments(gen_args_file_path: str) -> Tuple[Set[str], Set[str]]:
    """Get arguments and reply fields from generic_argument.idl and check validity."""
    arguments: Set[str] = set()
    reply_fields: Set[str] = set()

    with open(gen_args_file_path) as gen_args_file:
        parsed_idl_file = parser.parse(gen_args_file, gen_args_file_path,
                                       CompilerImportResolver([]))
        if parsed_idl_file.errors:
            parsed_idl_file.errors.dump_errors()
            raise ValueError(f"Cannot parse {gen_args_file_path}")
        for argument in parsed_idl_file.spec.symbols.get_generic_argument_list(
                "generic_args_api_v1").fields:
            arguments.add(argument.name)

        for reply_field in parsed_idl_file.spec.symbols.get_generic_reply_field_list(
                "generic_reply_fields_api_v1").fields:
            reply_fields.add(reply_field.name)

    return arguments, reply_fields


def check_generic_arguments_compatibility(old_gen_args_file_path: str, new_gen_args_file_path: str
                                          ) -> IDLCompatibilityErrorCollection:
    """Check IDL compatibility between old and new generic_argument.idl files."""
    # IDLCompatibilityContext takes in both 'old_idl_dir' and 'new_idl_dir',
    # but for generic_argument.idl, the parent directories aren't helpful for logging purposes.
    # Instead, we pass in "old generic_argument.idl" and "new generic_argument.idl"
    # to make error messages clearer.
    ctxt = IDLCompatibilityContext("old generic_argument.idl", "new generic_argument.idl",
                                   IDLCompatibilityErrorCollection())

    old_arguments, old_reply_fields = get_generic_arguments(old_gen_args_file_path)
    new_arguments, new_reply_fields = get_generic_arguments(new_gen_args_file_path)

    for old_argument in old_arguments:
        if old_argument not in new_arguments:
            ctxt.add_generic_argument_removed(old_argument, new_gen_args_file_path)

    for old_reply_field in old_reply_fields:
        if old_reply_field not in new_reply_fields:
            ctxt.add_generic_argument_removed_reply_field(old_reply_field, new_gen_args_file_path)

    return ctxt.errors


def main():
    """Run the script."""
    arg_parser = argparse.ArgumentParser(description=__doc__)
    arg_parser.add_argument("-v", "--verbose", action="count", help="Enable verbose logging")
    arg_parser.add_argument("--include", type=str, action="append",
                            help="Directory to search for IDL import files")
    arg_parser.add_argument("old_idl_dir", metavar="OLD_IDL_DIR",
                            help="Directory where old IDL files are located")
    arg_parser.add_argument("new_idl_dir", metavar="NEW_IDL_DIR",
                            help="Directory where new IDL files are located")
    args = arg_parser.parse_args()

    error_coll = check_compatibility(args.old_idl_dir, args.new_idl_dir, args.include)
    if error_coll.has_errors():
        sys.exit(1)

    old_basic_types_path = os.path.join(args.old_idl_dir, "mongo/idl/basic_types.idl")
    new_basic_types_path = os.path.join(args.new_idl_dir, "mongo/idl/basic_types.idl")
    error_reply_coll = check_error_reply(old_basic_types_path, new_basic_types_path, args.include)
    if error_reply_coll.has_errors():
        sys.exit(1)

    old_generic_args_path = os.path.join(args.old_idl_dir, "mongo/idl/generic_argument.idl")
    new_generic_args_path = os.path.join(args.new_idl_dir, "mongo/idl/generic_argument.idl")
    error_gen_args_coll = check_generic_arguments_compatibility(old_generic_args_path,
                                                                new_generic_args_path)
    if error_gen_args_coll.has_errors():
        sys.exit(1)


if __name__ == "__main__":
    main()
