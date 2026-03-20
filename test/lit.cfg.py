# -*- Python -*-

import os
import lit.formats
from lit.llvm import llvm_config

config.name = 'WIDTH-OPT'
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)
config.suffixes = ['.ll']
config.test_source_root = os.path.dirname(__file__)

tools = ["opt", "FileCheck"]
llvm_config.add_tool_substitutions(tools, config.llvm_tools_dir)

config.substitutions.append(('%shlibext', config.llvm_shlib_ext))
config.substitutions.append(('%shlibdir', config.llvm_shlib_dir))
