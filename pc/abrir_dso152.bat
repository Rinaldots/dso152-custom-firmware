@echo off
rem Abre a interface de captura do DSO152 no Microsoft Edge, que tem Web Serial
rem (o Firefox nao tem). Com o Chrome tambem funciona: abra o dso152.html nele.
start "" msedge "%~dp0dso152.html"
