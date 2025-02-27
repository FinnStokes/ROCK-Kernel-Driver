#ifndef AMDKCL_DRM_ATOMIC_HELPER_H
#define AMDKCL_DRM_ATOMIC_HELPER_H

#include <drm/drm_atomic_helper.h>


#if DRM_VERSION_CODE < DRM_VERSION(4, 7, 0)

static struct drm_encoder *
drm_atomic_helper_best_encoder(struct drm_connector *connector)
{
            WARN_ON(connector->encoder_ids[1]);
            return drm_encoder_find(connector->dev, NULL, connector->encoder_ids[0]);
}
#endif

#endif /* AMDKCL_DRM_ATOMIC_HELPERH */
