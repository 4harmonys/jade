project(
  'jade',
  'cpp',
  version : '0.1',
  meson_version : '>= 1.3.0',
  default_options : ['warning_level=3', 'cpp_std=c++17'],
)

llvm_dependency = dependency(
  'llvm',
  modules : ['core', 'support', 'irreader'],
  method : 'config-tool',
)

dependencies = [
  llvm_dependency,
]

sources = [
  'src/main.cpp',
  'src/lexer.cpp',
  'src/parser.cpp',
  'src/driver.cpp',
]

jade_executable = executable(
  'jade',
  sources,
  install : true,
  dependencies : dependencies,
)

test('basic', jade_executable)
