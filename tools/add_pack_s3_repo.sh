#!/bin/bash
set -e

# manual configuration
if [ "$BUCKET" == "" ]; then
    echo "ERROR: need to set BUCKET env variable to any like <1_10,2_2,2_3,2x>"
    exit 1
fi
branch=$BUCKET

# flag to force DEB based distributions update
update_dists=0

# configuration
if [ "$OS" == "" ]; then
    echo "ERROR: need to set OS env variable to any single of <ubuntu,debian,centos|el,fedora> OS"
    exit 1
fi
os=$OS
if [ "$os" == "el" ]; then
    os=centos
fi
if [ "$DISTS" == "" ]; then
    echo "ERROR: need to set DISTS env variable to any set os multi either single number of distributions depending OS:"
    echo "    ubuntu(multi): <bionic,cosmic,disco,trusty,xenial>"
    echo "    debian(multi): <stretch,buster>"
    echo "    centos(single): <6,7,8>"
    echo "    fedora(single): <27,28,29,30>"
    exit 1
fi

product=$PRODUCT
if [ "$PRODUCT" == "" ]; then
    product=tarantool
    echo "WARNING: set PRODUCT env variable to default '$product' value"
fi
proddir=`echo $product | head -c 1`

aws='aws --endpoint-url https://hb.bizmrg.com'
s3="s3://tarantool_repo/$branch/$os"

# get the path with binaries
repo=$1

function pack_deb {
    dists=$DISTS

    # we need to push packages into 'main' repository only
    component=main

    # debian has special directory 'pool' for packages
    debdir=pool

    # get packages from pointed location either mirror path
    if [ "$repo" == "" ] ; then
        repo=/var/spool/apt-mirror/mirror/packagecloud.io/tarantool/$branch/$os
    fi
    if [ ! -d $repo/$debdir ] && ! ls $repo/*.deb build/*.dsc build/*.tar.*z >/dev/null 2>&1 ; then
        echo "ERROR: Current '$repo' has:"
        ls -al $repo
        echo "Usage with packages: $0 [path with *.deb *.dsc *.tar.*z files]"
        echo "Usage with repositories: $0 [path to repository with '$debdir' directory in root path]"
        exit 1
    fi

    # temporary lock the publication to the repository
    ws=/tmp/tarantool_repo_s3_${branch}_${os}
    wslock=$ws.lock
    lockfile -l 1000 $wslock

    # create temporary workspace with repository copy
    rm -rf $ws
    mkdir -p $ws
        if ls $repo/*.deb $repo/*.dsc $repo/*.tar.*z >/dev/null 2>&1 ; then
        # only single distribution is possible to use in this way
            repopath=$ws/pool/${dists}/main/$proddir/$product
            mkdir -p ${repopath}
            cp $repo/*.deb $repo/*.dsc build/*.tar.*z $repopath/.
    else
        cp -rf $repo/$debdir $ws/.
    fi
    cd $ws

    # create the configuration file
    confpath=$ws/conf
    rm -rf $confpath
    mkdir -p $confpath

    for dist in $dists ; do
        cat <<EOF >>$confpath/distributions
Origin: Tarantool
Label: tarantool.org
Suite: stable
Codename: $dist
Architectures: amd64 source
Components: main
Description: Unofficial Ubuntu Packages maintained by Tarantool
SignWith: 91B625E5
DebIndices: Packages Release . .gz .bz2
UDebIndices: Packages . .gz .bz2
DscIndices: Sources Release .gz .bz2

EOF
done

    # create standalone repository with separate components
    for dist in $dists ; do
        echo =================== DISTRIBUTION: $dist =========================
        updated_deb=0
        updated_dsc=0

        # 1(binaries). use reprepro tool to generate Packages file
        for deb in $ws/$debdir/$dist/$component/*/*/*.deb ; do
        [ -f $deb ] || continue
        locdeb=`echo $deb | sed "s#^$ws\/##g"`
        echo "DEB: $deb"
        # register DEB file to Packages file
        reprepro -Vb . includedeb $dist $deb
        # reprepro copied DEB file to local component which is not needed
        rm -rf $debdir/$component
        # to have all packages avoid reprepro set DEB file to its own registry
        rm -rf db
        # copy Packages file to avoid of removing by the new DEB version
        for packages in dists/$dist/$component/binary-*/Packages ; do
            if [ ! -f $packages.saved ] ; then
            # get the latest Packages file from S3
            $aws s3 ls "$s3/$packages" 2>/dev/null && \
                $aws s3 cp --acl public-read \
                "$s3/$packages" $packages.saved || \
                touch $packages.saved
            fi
            # check if the DEB file already exists in Packages from S3
            if grep "^`grep "^SHA256: " $packages`$" $packages.saved ; then
            echo "WARNING: DEB file already registered in S3!"
            continue
            fi
            # store the new DEB entry
            cat $packages >>$packages.saved
            # save the registered DEB file to S3
            $aws s3 cp --acl public-read $deb $s3/$locdeb
            updated_deb=1
        done
        done

        # 1(sources). use reprepro tool to generate Sources file
        for dsc in $ws/$debdir/$dist/$component/*/*/*.dsc ; do
        [ -f $dsc ] || continue
        locdsc=`echo $dsc | sed "s#^$ws\/##g"`
        echo "DSC: $dsc"
        # register DSC file to Sources file
        reprepro -Vb . includedsc $dist $dsc
        # reprepro copied DSC file to component which is not needed
        rm -rf $debdir/$component
        # to have all sources avoid reprepro set DSC file to its own registry
        rm -rf db
        # copy Sources file to avoid of removing by the new DSC version
        sources=dists/$dist/$component/source/Sources
        if [ ! -f $sources.saved ] ; then
            # get the latest Sources file from S3
            $aws s3 ls "$s3/$sources" && \
            $aws s3 cp --acl public-read "$s3/$sources" $sources.saved || \
            touch $sources.saved
        fi
        # WORKAROUND: unknown why, but reprepro doesn`t save the Sources file
        gunzip -c $sources.gz >$sources
        # check if the DSC file already exists in Sources from S3
        hash=`grep '^Checksums-Sha256:' -A3 $sources | \
            tail -n 1 | awk '{print $1}'`
        if grep " $hash .*\.dsc$" $sources.saved ; then
            echo "WARNING: DSC file already registered in S3!"
            continue
        fi
        # store the new DSC entry
        cat $sources >>$sources.saved
        # save the registered DSC file to S3
        $aws s3 cp --acl public-read $dsc $s3/$locdsc
        tarxz=`echo $locdsc | sed 's#\.dsc$#.debian.tar.xz#g'`
        $aws s3 cp --acl public-read $ws/$tarxz "$s3/$tarxz"
        orig=`echo $locdsc | sed 's#-1\.dsc$#.orig.tar.xz#g'`
        $aws s3 cp --acl public-read $ws/$orig "$s3/$orig"
        updated_dsc=1
        done

        # check if any DEB/DSC files were newly registered
        [ "$update_dists" == "0" -a "$updated_deb" == "0" -a "$updated_dsc" == "0" ] && \
        continue || echo "Updating dists"

        # finalize the Packages file
        for packages in dists/$dist/$component/binary-*/Packages ; do
        mv $packages.saved $packages
        done

        # 2(binaries). update Packages file archives
        for packpath in dists/$dist/$component/binary-* ; do
        cd $packpath
        sed "s#Filename: $debdir/$component/#Filename: $debdir/$dist/$component/#g" -i Packages
        bzip2 -c Packages >Packages.bz2
        gzip -c Packages >Packages.gz
        cd -
        done

        # 2(sources). update Sources file archives
        cd dists/$dist/$component/source
        sed "s#Directory: $debdir/$component/#Directory: $debdir/$dist/$component/#g" -i Sources
        bzip2 -c Sources >Sources.bz2
        gzip -c Sources >Sources.gz
        cd -

        # 3. update checksums of the Packages* files in *Release files
        cd dists/$dist
        for file in `grep " $component/" Release | awk '{print $3}' | sort -u` ; do
        sz=`stat -c "%s" $file`
        md5=`md5sum $file | awk '{print $1}'`
        sha1=`sha1sum $file | awk '{print $1}'`
        sha256=`sha256sum $file | awk '{print $1}'`
        awk 'BEGIN{c = 0} ; {
            if ($3 == p) {
                c = c + 1
                if (c == 1) {print " " md  " " s " " p}
                if (c == 2) {print " " sh1 " " s " " p}
                if (c == 3) {print " " sh2 " " s " " p}
            } else {print $0}
            }' p="$file" s="$sz" md="$md5" sh1="$sha1" sh2="$sha256" \
            Release >Release.new
        mv Release.new Release
        done
        # resign the selfsigned InRelease file
        rm -rf InRelease
        gpg --clearsign -o InRelease Release
        # resign the Release file
        rm -rf Release.gpg
        gpg -abs -o Release.gpg Release
        cd -

        # 4. sync the latest distribution path changes to S3
        $aws s3 sync --acl public-read dists/$dist "$s3/dists/$dist"
    done

    # unlock the publishing
    rm -rf $wslock
}

function pack_rpm {
    release=$DISTS

    # get packages from pointed location either default Docker's mirror
    if [ "$repo" == "" ] ; then
        repo=.
    fi
    if ! ls $repo/*.rpm >/dev/null 2>&1 ; then
        echo "ERROR: Current '$repo' has:"
        ls -al $repo
        echo "Usage: $0 [path with *.rpm files]"
        exit 1
    fi

    # temporary lock the publication to the repository
    ws=/tmp/tarantool_repo_s3_${branch}_${os}_${release}
    wslock=$ws.lock
    lockfile -l 1000 $wslock

    # create temporary workspace with packages copies
    rm -rf $ws
    mkdir -p $ws
    cp $repo/*.rpm $ws/.
    cd $ws

    # set the paths
    if [ "$os" == "centos" ]; then
        repopath=$release/os/x86_64
        rpmpath=Packages
    elif [ "$os" == "fedora" ]; then
        repopath=releases/$release/Everything/x86_64/os
        rpmpath=Packages/$proddir
    fi
    packpath=$repopath/$rpmpath

    # prepare local repository with packages
    mkdir -p $packpath
    mv *.rpm $packpath/.
    cd $repopath

    # copy the current metadata files from S3
    mkdir repodata.base
    for file in `$aws s3 ls $s3/$repopath/repodata/ | awk '{print $NF}'` ; do
        $aws s3 ls $s3/$repopath/repodata/$file || continue
        $aws s3 cp $s3/$repopath/repodata/$file repodata.base/$file
    done

    # create the new repository metadata files
    createrepo --no-database --update --workers=2 --compress-type=gz --simple-md-filenames .
    mv repodata repodata.adding

    # merge metadata files
    mkdir repodata
    head -n 2 repodata.adding/repomd.xml >repodata/repomd.xml
    for file in filelists.xml other.xml primary.xml ; do
        # 1. take the 1st line only - to skip the line with number of packages which is not needed
        zcat repodata.adding/$file.gz | head -n 1 >repodata/$file
        # 2. take 2nd line with metadata tag and update the packages number in it
        packsold=0
        if [ -f repodata.base/$file.gz ] ; then
        packsold=`zcat repodata.base/$file.gz | head -n 2 | tail -n 1 | sed 's#.*packages="\(.*\)".*#\1#g'`
        fi
        packsnew=`zcat repodata.adding/$file.gz | head -n 2 | tail -n 1 | sed 's#.*packages="\(.*\)".*#\1#g'`
        packs=$(($packsold+$packsnew))
        zcat repodata.adding/$file.gz | head -n 2 | tail -n 1 | sed "s#packages=\".*\"#packages=\"$packs\"#g" >>repodata/$file
        # 3. take only 'package' tags from new file
        zcat repodata.adding/$file.gz | tail -n +3 | head -n -1 >>repodata/$file
        # 4. take only 'package' tags from old file if exists
        if [ -f repodata.base/$file.gz ] ; then
        zcat repodata.base/$file.gz | tail -n +3 | head -n -1 >>repodata/$file
        fi
        # 5. take the last closing line with metadata tag
        zcat repodata.adding/$file.gz | tail -n 1 >>repodata/$file

        # get the new data
        chsnew=`sha256sum repodata/$file | awk '{print $1}'`
        sz=`stat --printf="%s" repodata/$file`
        gzip repodata/$file
        chsgznew=`sha256sum repodata/$file.gz | awk '{print $1}'`
        szgz=`stat --printf="%s" repodata/$file.gz`
        timestamp=`date +%s -r repodata/$file.gz`

        # add info to repomd.xml file
        name=`echo $file | sed 's#\.xml$##g'`
        echo "<data type=\"$name\">" >>repodata/repomd.xml
        echo "  <checksum type=\"sha256\">$chsgznew</checksum>" >>repodata/repomd.xml
        echo "  <open-checksum type=\"sha256\">$chsnew</open-checksum>" >>repodata/repomd.xml
        echo "  <location href=\"repodata/$file.gz\"/>" >>repodata/repomd.xml
        echo "  <timestamp>$timestamp</timestamp>" >>repodata/repomd.xml
        echo "  <size>$szgz</size>" >>repodata/repomd.xml
        echo "  <open-size>$sz</open-size>" >>repodata/repomd.xml
        echo "</data>" >>repodata/repomd.xml
    done
    tail -n 1 repodata.adding/repomd.xml >>repodata/repomd.xml
    gpg --detach-sign --armor repodata/repomd.xml

    # copy the packages to S3
    for file in $rpmpath/*.rpm ; do
        $aws s3 cp --acl public-read $file "$s3/$repopath/$file"
    done

    # update the metadata at the S3
    $aws s3 sync --acl public-read repodata "$s3/$repopath/repodata"

    # unlock the publishing
    rm -rf $wslock
}

if [ "$os" == "ubuntu" -o "$os" == "debian" ]; then
    pack_deb
elif [ "$os" == "centos" -o "$os" == "fedora" ]; then
    pack_rpm
else
    echo "USAGE: given OS '$OS' is not supported, use any single from the list: ubuntu, debian, centos|el, fedora"
    exit 1
fi
