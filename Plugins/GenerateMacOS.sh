#!/bin/bash

CamomileInst=Camomile
CamomileFx=CamomileFx

VstExtension=vst
Vst3Extension=vst3
AuExtension=component

VstPath=$HOME/Library/Audio/Plug-Ins/VST
Vst3Path=$HOME/Library/Audio/Plug-Ins/VST3
AuPath=$HOME/Library/Audio/Plug-Ins/Components

ThisPath="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

install_plugin() {
    if [ -d $VstPath/$PatchName ]; then
        rm -rf $VstPath/$PatchName
    fi
    cp -rf $ThisPath/Builds/$PatchName/ $VstPath/$PatchName
    rm -rf $ThisPath/Builds/$PatchName
}

install_all_plugins() {
    echo -n "install "
    for Patch in $ThisPath/Builds/*
    do
      PatchName=${Patch##*/}
      echo -n $PatchName " "
      install_plugin
    done
    echo " "
}

generate_plugin() {
    if [ -d $ThisPath/Camomile/$CamomileName.$1 ]; then
        if [ -d $ThisPath/Builds/$PatchName.$1 ]; then
            rm -rf $ThisPath/Builds/$PatchName.$1
        fi
        cp -rf $ThisPath/Camomile/$CamomileName.$1/ $ThisPath/Builds/$PatchName.$1
        cp -rf $PatchesPath/$PatchName/ $ThisPath/Builds/$PatchName.$1/Contents/Resources
        echo -n $1 " "
    fi
}

generate_plugins_in_folder() {
    if [ "$1" == "Effect" ]; then
        CamomileName=$CamomileFx
    else
        CamomileName=$CamomileInst
    fi
    for Patch in $PatchesPath/*
    do
      PatchName=${Patch##*/}
      if [ -d $PatchesPath/$PatchName ]; then
          echo -n $PatchName
          echo -n " ($1):"
          generate_plugin $VstExtension
          generate_plugin $Vst3Extension
          generate_plugin $AuExtension
          echo ""
      fi
    done
}

if [ ! -d $ThisPath/Builds ]; then
    mkdir $ThisPath/Builds
fi

echo Camomile Generates the new plugins from $ThisPath
PatchesPath=$ThisPath/Effects
generate_plugins_in_folder Effect
PatchesPath=$ThisPath/Instruments
generate_plugins_in_folder Instrument

if [ "$1" == "install" ]; then
    install_all_plugins
fi
