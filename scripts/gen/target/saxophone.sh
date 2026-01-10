#!/usr/bin/env bash

sax_register_chromatic=$($yaml_parser "$yaml_file" saxophone.registerChromatic)

if [[ "$sax_register_chromatic" == "true" ]]
then
    printf "%s\n" "list(APPEND $cmake_defines_var PROJECT_TARGET_SAX_REGISTER_CHROMATIC)" >> "$out_cmakelists"
fi
