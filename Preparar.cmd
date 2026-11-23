@echo off
rem Abre a tela de preparacao: escolha sua ISO ou pasta do NieR e o resto e automatico.
start "" powershell -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0scripts\setup.ps1"
