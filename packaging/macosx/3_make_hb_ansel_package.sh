#!/bin/bash
#
# Script to create an application bundle from build files
#
# Usage note:   Define CODECERT to properly sign app bundle. As example:
#               $ export CODECERT="developer@apple.id"
#               The mail address is the email/id of your developer certificate.
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Go to directory of script
scriptDir=$(dirname "$0")
cd "$scriptDir"/

# Define base variables
buildDir="../../build/macosx"
dtPackageDir="$buildDir"/package
dtAppName="Ansel"
dtWorkingDir="$dtPackageDir"/"$dtAppName".app
dtResourcesDir="$dtWorkingDir"/Contents/Resources
dtExecDir="$dtWorkingDir"/Contents/MacOS
dtExecutables=$(echo "$dtExecDir"/ansel{,-chart,-cli,-cltest,-generate-cache,-rs-identify,-curve-tool,-noiseprofile})
homebrewHome=$(brew --prefix)

# Install direct and transitive dependencies
function install_dependencies {
    local hbDependencies
    local absolutePath=$(dirname $(realpath "$1"))

    # Get dependencies of current executable
    local oToolLDependencies=$(otool -L "$1" 2>/dev/null | grep compatibility | cut -d\( -f1 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' | uniq)
    # Handle library relative paths
    oToolLDependencies=$(echo "$oToolLDependencies" | sed "s#@loader_path#${absolutePath}#")
    oToolLDependencies=$(echo "$oToolLDependencies" | sed "s#@rpath#${absolutePath}#")
    # Filter for homebrew dependencies
    if [[ "$oToolLDependencies" == *"$homebrewHome"* ]]; then
        hbDependencies=$(echo "$oToolLDependencies" | grep "$homebrewHome")
    fi

    # Check for any homebrew dependencies
    if [[ -n "$hbDependencies" && "$hbDependencies" != "" ]]; then

        # Iterate over homebrew dependencies to install them accordingly
        for hbDependency in $hbDependencies; do
            # Skip dependency if it is a dependency of itself
            if [[ "$hbDependency" != "$1" ]]; then

                # Store file name
                dynDepOrigFile=$(basename "$hbDependency")
                dynDepTargetFile="$dtResourcesDir/lib/$dynDepOrigFile"

                # Install dependency if not yet existant
                if [[ ! -f "$dynDepTargetFile" ]]; then
                    echo "Installing dependency <$hbDependency> of <$1>."

                    # Copy dependency as not yet existant
                    cp -L "$hbDependency" "$dynDepTargetFile"

                    # Handle transitive dependencies
                    install_dependencies "$hbDependency"
                fi
            fi
        done

    fi
}

# Reset executable path to relative path
# Background: see e.g. http://clarkkromenaker.com/post/library-dynamic-loading-mac/
function reset_exec_path {
    local hbDependencies

    # Get shared libraries used of current executable
    oToolLDependencies=$(otool -L "$1" 2>/dev/null | grep compatibility | cut -d\( -f1 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' | uniq)

    # Handle libansel.dylib
    if [[ "$oToolLDependencies" == *"@rpath/libansel.dylib"* && "$1" != *"libansel.dylib"* ]]; then
	    oToolLoaderPath=$(otool -l $1 | grep @loader|cut -f11 -d' ')
        echo "Resetting loader path for libansel.dylib of <$1>"
        install_name_tool -rpath $oToolLoaderPath @loader_path/../Resources/lib "$1"
    fi

    # Handle library relative paths
    oToolLDependencies=$(echo "$oToolLDependencies" | sed "s#@loader_path/[../]*opt/#${homebrewHome}/opt/#")

    # Filter for any homebrew specific paths
    if [[ "$oToolLDependencies" == *"$homebrewHome"* ]]; then
        hbDependencies=$(echo "$oToolLDependencies" | grep "$homebrewHome")
    fi

    # Check for any homebrew dependencies
    if [[ -n "$hbDependencies" && "$hbDependencies" != "" ]]; then

        # Iterate over homebrew dependencies to reset path accordingly
        for hbDependency in $hbDependencies; do

            # Store file name
            dynDepOrigFile=$(basename "$hbDependency")
            dynDepTargetFile="$dtResourcesDir/lib/$dynDepOrigFile"

            # Set correct executable path
            install_name_tool -change "$hbDependency" "@executable_path/../Resources/lib/$dynDepOrigFile" "$1" || true

            # Check for loader path
            oToolLoader=$(otool -L "$1" 2>/dev/null | grep '@loader_path' | grep $dynDepOrigFile | cut -d\( -f1 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' ) || true
            if [[ -n "$oToolLoader" ]]; then
                echo "Resetting loader path for dependency <$hbDependency> of <$1>"
                oToolLoaderNew=$(echo $oToolLoader | sed "s#@loader_path/##" | sed "s#../../../../opt/.*##")
                install_name_tool -change "$oToolLoader" "@loader_path/${oToolLoaderNew}${dynDepOrigFile}" "$1"  || true
            fi

        done

    fi

    # Get shared library id name of current executable
    oToolDDependencies=$(otool -D "$1" 2>/dev/null | sort | uniq)

    # Set correct ID to new destination if required
    if [[ "$oToolDDependencies" == *"$homebrewHome"* ]]; then

        # Store file name
        libraryOrigFile=$(basename "$1")

        echo "Resetting library ID of <$1>"

        # Set correct library id
        install_name_tool -id "@executable_path/../Resources/lib/$libraryOrigFile" "$1"
    fi
}

# Search and install any translation files
function install_translations {

    # Find relevant translation files
    translationFiles=$(find "$homebrewHome"/share/locale -name "$1".mo)

    for srcTranslFile in $translationFiles; do

        # Define target filename
        targetTranslFile=${srcTranslFile//"$homebrewHome"/"$dtResourcesDir"}

        # Create directory if not yet existing
        targetTranslDir=$(dirname "$targetTranslFile")
        if [[ ! -d "$targetTranslDir" ]]; then
            mkdir -p "$targetTranslDir"
        fi
        # Copy translation file
        cp -L "$srcTranslFile" "$targetTranslFile"
    done
}

# Install share directory
function install_share {

    # Define source and target directory
    srcShareDir="$homebrewHome/share/$1"
    targetShareDir="$dtResourcesDir/share/"

    # Copy share directory
    cp -RL "$srcShareDir" "$targetShareDir"
}

# Check for previous attempt and clean
if [[ -d "$dtWorkingDir" ]]; then
    echo "Deleting directory $dtWorkingDir ... "
    chown -R "$USER" "$dtWorkingDir"
    rm -Rf "$dtWorkingDir"
fi

# Create basic structure
mkdir -p "$dtExecDir"
mkdir -p "$dtResourcesDir"/share/applications
mkdir -p "$dtResourcesDir"/etc/gtk-3.0
mkdir -p "$dtResourcesDir"/fonts

# exiv2 expects the localization files in '../share/locale'
ln -s "Resources/share" "$dtWorkingDir"/Contents/share

# Add basic elements
cp Info.plist "$dtWorkingDir"/Contents/
echo "APPL$dtAppName" >>"$dtWorkingDir"/Contents/PkgInfo

# Set version information
sed -i -e 's|{VERSION}|'$(git describe --tags --long --match '*[0-9.][0-9.][0-9]' | cut -d- -f2 | sed 's/^\([0-9]*\.[0-9]*\)$/\1.0/')'|' "$dtWorkingDir"/Contents/Info.plist
sed -i -e 's|{COMMITS}|'$(git describe --tags --long --match '*[0-9.][0-9.][0-9]' | cut -d- -f3)'|' "$dtWorkingDir"/Contents/Info.plist

# Generate settings.ini
echo "[Settings]
gtk-icon-theme-name = Adwaita
" >"$dtResourcesDir"/etc/gtk-3.0/settings.ini

# Add ansel executables
cp "$buildDir"/bin/ansel{,-cli,-cltest,-generate-cache,-rs-identify} "$dtExecDir"/

# Add ansel tools if existent
if [[ -d libexec/ansel/tools ]]; then
    cp "$buildDir"/libexec/ansel/tools/* "$dtExecDir"/
fi

# Add ansel directories
cp -R "$buildDir"/{lib,share} "$dtResourcesDir"/

# Install homebrew dependencies of ansel executables
for dtExecutable in $dtExecutables; do
    if [[ -f "$dtExecutable" ]]; then
        install_dependencies "$dtExecutable"
    fi
done

# Add homebrew shared objects
dtSharedObjDirs="gtk-3.0 gdk-pixbuf-2.0 gio ImageMagick"
for dtSharedObj in $dtSharedObjDirs; do
    cp -LR "$homebrewHome"/lib/"$dtSharedObj" "$dtResourcesDir"/lib/
done

# Add homebrew translations
dtTranslations="gtk30 gtk30-properties gtk-mac-integration iso_639-2 exiv2"
for dtTranslation in $dtTranslations; do
    install_translations "$dtTranslation"
done

# Add homebrew share directories
dtShares="lensfun icons iso-codes mime"
for dtShare in $dtShares; do
    install_share "$dtShare"
done

# Update icon caches
gtk3-update-icon-cache -f "$dtResourcesDir"/share/icons/Adwaita
gtk3-update-icon-cache -f "$dtResourcesDir"/share/icons/hicolor

# Try updating lensfun
lensfun-update-data || true
lfLatestData="$HOME"/.local/share/lensfun/updates/version_1
if [[ -d "$lfLatestData" ]]; then
    echo "Adding latest lensfun data from $lfLatestData."
    cp -R "$lfLatestData" "$dtResourcesDir"/share/lensfun/
fi

# Add glib gtk settings schemas
glibSchemasDir="$dtResourcesDir"/share/glib-2.0/schemas
if [[ ! -d "$glibSchemasDir" ]]; then
    mkdir -p "$glibSchemasDir"
fi
cp -L "$homebrewHome"/share/glib-2.0/schemas/org.gtk.Settings.*.gschema.xml "$glibSchemasDir"/
# Compile glib schemas
glib-compile-schemas "$dtResourcesDir"/share/glib-2.0/schemas/

# Define gtk-query-immodules-3.0
immodulesCacheFile="$dtResourcesDir"/lib/gtk-3.0/3.0.0/immodules.cache
hbGtk3Path=$(brew info gtk+3|grep "/`pkg-config --modversion gtk+-3.0`"|cut -f1 -d' ')
sed -i -e "s#$hbGtk3Path/lib/gtk-3.0/3.0.0/immodules#@executable_path/../Resources/lib/gtk-3.0/3.0.0/immodules#g" "$immodulesCacheFile"
sed -i -e "s#$hbGtk3Path/share/locale#@executable_path/../Resources/share/locale#g" "$immodulesCacheFile"
# Rename and move it to the right place
mv "$immodulesCacheFile" "$dtResourcesDir"/etc/gtk-3.0/gtk.immodules

# Define gdk-pixbuf-query-loaders
loadersCacheFile="$dtResourcesDir"/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache
sed -i -e "s#$homebrewHome/lib/gdk-pixbuf-2.0/2.10.0/loaders#@executable_path/../Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders#g" "$loadersCacheFile"
# Move it to the right place
mv "$loadersCacheFile" "$dtResourcesDir"/etc/gtk-3.0/

# ImageMagick config files
cp -R $homebrewHome/Cellar/imagemagick/*/etc $dtResourcesDir

# Install homebrew dependencies of lib subdirectories
dtLibFiles=$(find -E "$dtResourcesDir"/lib/*/* -regex '.*\.(so|dylib)')
for dtLibFile in $dtLibFiles; do
    install_dependencies "$dtLibFile"
done

# Reset executable paths to relative path
dtExecFiles="$dtExecutables"
dtExecFiles+=" "
dtExecFiles+=$(find -E "$dtResourcesDir"/lib -regex '.*\.(so|dylib)')
for dtExecFile in $dtExecFiles; do
    if [[ -f "$dtExecFile" ]]; then
        reset_exec_path "$dtExecFile"
    fi
done

# Add gtk files
cp defaults.list "$dtResourcesDir"/share/applications/
cp open.desktop "$dtResourcesDir"/share/applications/

# Add gtk Mac theme (to enable default macos keyboard shortcuts)
if [[ ! -d "$dtResourcesDir"/share/themes/Mac/gtk-3.0 ]]; then
    mkdir -p "$dtResourcesDir"/share/themes/Mac/gtk-3.0
fi
cp -L "$homebrewHome"/share/themes/Mac/gtk-3.0/gtk-keys.css "$dtResourcesDir"/share/themes/Mac/gtk-3.0/

# Add fonts
cp fonts/*  "$dtResourcesDir"/fonts/

# Patch ansel.css - Solving font issue with Roboto condensed
patch "$dtResourcesDir"/share/ansel/themes/ansel.css ansel.css.patch

# Create Icon file
if [ -d "$buildDir"/Icons.iconset ]; then
    rm -R "$buildDir/Icons.iconset"
fi
mkdir "$buildDir"/Icons.iconset
rsvg-convert -h 644 ../../data/pixmaps/scalable/ansel.svg > "$buildDir/Icons.iconset/icon_512x512.png"
magick mogrify -crop 512x512+66+66 "$buildDir/Icons.iconset/icon_512x512.png"
cp  "$buildDir/Icons.iconset/icon_512x512.png" "$buildDir/Icons.iconset/icon_256x256@2.png"
rsvg-convert -h 322 ../../data/pixmaps/scalable/ansel.svg > "$buildDir/Icons.iconset/icon_256x256.png"
magick mogrify -crop 256x256+33+33 "$buildDir/Icons.iconset/icon_256x256.png"
cp  "$buildDir/Icons.iconset/icon_256x256.png" "$buildDir/Icons.iconset/icon_128x128@2.png"
rsvg-convert -h 162 ../../data/pixmaps/scalable/ansel.svg > "$buildDir/Icons.iconset/icon_128x128.png"
magick mogrify -crop 128x128+17+17 "$buildDir/Icons.iconset/icon_128x128.png"
rsvg-convert -h 40 ../../data/pixmaps/scalable/ansel.svg > "$buildDir/Icons.iconset/icon_32x32.png"
magick mogrify -crop 32x32+4+4 "$buildDir/Icons.iconset/icon_32x32.png"
cp  "$buildDir/Icons.iconset/icon_32x32.png" "$buildDir/Icons.iconset/icon_16x168@2.png"
rsvg-convert -h 20 ../../data/pixmaps/scalable/ansel.svg > "$buildDir/Icons.iconset/icon_16x16.png"
magick mogrify -crop 16x16+2+2 "$buildDir/Icons.iconset/icon_16x16.png"
if [ -f "$buildDir/Icons.icns" ]; then
    rm "$buildDir/Icons.icns"
fi
iconutil -c icns "$buildDir/Icons.iconset"
cp "$buildDir/Icons.icns" "$dtResourcesDir"/

# Sign app bundle
if [ -n "$CODECERT" ]; then
    # Use certificate if one has been provided
    find ${dtPackageDir}/"$dtAppName".app/Contents/Resources/lib -type f -exec codesign --verbose --force --options runtime -i "org.ansel" -s "${CODECERT}" \{} \;
    codesign --deep --verbose --force --options runtime -i "photos.ansel" -s "${CODECERT}" ${dtPackageDir}/"$dtAppName".app
else
    # Use ad-hoc signing and preserve metadata
    find ${dtPackageDir}/"$dtAppName".app/Contents/Resources/lib -type f -exec codesign --verbose --force --preserve-metadata=entitlements,requirements,flags,runtime -i "org.ansel" -s - \{} \;
    codesign --deep --verbose --force --preserve-metadata=entitlements,requirements,flags,runtime -i "photos.ansel" -s - ${dtPackageDir}/"$dtAppName".app
fi
