# Windows(MSYS2) 部署脚本：把运行所需的 DLL、Qt 插件、ffmpeg/ffprobe 收集到 exe 所在目录。
# 用法：cmake -DEXE=<exe> -DPREFIX=<MSYS2 前缀> -P deploy.cmake
get_filename_component(OUTDIR "${EXE}" DIRECTORY)

# 1) Qt 的 DLL 与插件（platforms/styles/imageformats 等）
find_program(WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${PREFIX}/bin" "${PREFIX}/lib/qt6/bin")
if(WINDEPLOYQT)
  execute_process(COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler
                          --no-opengl-sw --no-quick-import "${EXE}"
                  WORKING_DIRECTORY "${OUTDIR}" RESULT_VARIABLE _r)
else()
  message(WARNING "未找到 windeployqt，跳过 Qt 插件部署")
endif()

# 2) ffmpeg / ffprobe
set(_tools "")
foreach(t ffmpeg.exe ffprobe.exe)
  if(EXISTS "${PREFIX}/bin/${t}")
    file(COPY "${PREFIX}/bin/${t}" DESTINATION "${OUTDIR}")
    list(APPEND _tools "${PREFIX}/bin/${t}")
  else()
    message(WARNING "未找到 ${PREFIX}/bin/${t}（请安装 mingw-w64-*-ffmpeg）")
  endif()
endforeach()

# 3) 其余依赖 DLL（libmpv、libgcc、libstdc++、libwinpthread、ffmpeg 的 libav* 等），递归解析
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${EXE}" ${_tools}
  RESOLVED_DEPENDENCIES_VAR _res
  UNRESOLVED_DEPENDENCIES_VAR _unres
  DIRECTORIES "${PREFIX}/bin"
  PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
  POST_EXCLUDE_REGEXES ".*[/\\\\][Ww]indows[/\\\\].*" ".*[/\\\\]system32[/\\\\].*")
foreach(f ${_res})
  file(COPY "${f}" DESTINATION "${OUTDIR}")
endforeach()
if(_unres)
  message(STATUS "未解析的依赖（通常是系统 DLL，可忽略）：${_unres}")
endif()
message(STATUS "部署完成：${OUTDIR}")
