@echo off
rem Same as record.cmd, with the experimental 60 fps patch turned on.
set "NIER_EXTRA_ARGS=--fps_60=true"
call "%~dp0record.cmd" sessao60
