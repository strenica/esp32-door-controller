FROM php:8.4-apache

# pdo_mysql je obvezen; mbstring potrebuje libonig-dev (če v sliki še ni vgrajen).
RUN docker-php-ext-install -j"$(nproc)" pdo_mysql \
 && if ! php -m | grep -qi '^mbstring$'; then \
      apt-get update \
      && apt-get install -y --no-install-recommends libonig-dev \
      && docker-php-ext-install -j"$(nproc)" mbstring \
      && rm -rf /var/lib/apt/lists/*; \
    fi

# Firmware je ~1.5 MB; privzeti 2 MB je pretesan (glej README.md).
COPY uploads.ini /usr/local/etc/php/conf.d/zz-uploads.ini
