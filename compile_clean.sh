#!/bin/bash
# Clear cached external_components and packages
rm -rf /config/esphome-intercom/.esphome/external_components/852a4dc7/
rm -rf /config/esphome-intercom/.esphome/packages/852a4dc7/

# Unset proxy vars that break PlatformIO
unset HTTP_PROXY HTTPS_PROXY http_proxy https_proxy ALL_PROXY all_proxy
export NO_PROXY="*"

# Compile
esphome compile /config/esphome-intercom/waveshare-s3-touch-4b-va-intercom.yaml
echo "EXIT_CODE=$?"
