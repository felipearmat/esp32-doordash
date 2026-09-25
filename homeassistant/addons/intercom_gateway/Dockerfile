ARG BUILD_FROM
FROM $BUILD_FROM

# speexdsp-dev/swig/build-base/python3-dev: only needed to COMPILE the
# Python binding for libspeexdsp (the 'speexdsp' PyPI package, a SWIG
# wrapper around speex_echo_state — this is what does the AEC). speexdsp
# (without -dev) stays installed as the runtime shared library; the rest is
# removed at the end to keep the image small.
# Versions pinned deliberately: an unpinned 'pip install' would silently
# pull whatever is newest at build time, which can break the add-on on a
# rebuild months later or pull in a compromised release. Bump these
# intentionally when there's a reason to.
#
# speexdsp 0.1.1 ships an old SWIG wrapper that loads its extension through
# the 'imp' module, removed in Python 3.12. The sed swaps that single call for
# a relative import of the same extension, and the final python3 -c makes the
# build fail right here if the binding still can't be imported.
RUN apk add --no-cache python3 py3-pip speexdsp \
    && apk add --no-cache --virtual .build-deps \
         build-base python3-dev swig speexdsp-dev \
    && pip3 install --break-system-packages aiohttp==3.14.3 speexdsp==0.1.1 \
    && sed -i 's/^\( *\)_speexdsp = swig_import_helper()/\1from . import _speexdsp/' \
         /usr/lib/python3*/site-packages/speexdsp/speexdsp.py \
    && apk del .build-deps \
    && python3 -c "from speexdsp import EchoCanceller; EchoCanceller.create(160, 2000, 8000)"

COPY server.py /server.py
CMD ["python3", "/server.py"]
