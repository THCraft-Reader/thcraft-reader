/****************************************************************************
 *
 * afmodule.h
 *
 *   Auto-fitter module implementation (specification).
 *
 * Copyright (C) 2003-2026 by
 * David Turner, Robert Wilhelm, and Werner Lemberg.
 *
 * This file is part of the FreeType project, and may only be used,
 * modified, and distributed under the terms of the FreeType project
 * license, LICENSE.TXT.  By continuing to use, modify, or distribute
 * this file you indicate that you have read the license and
 * understand and accept it fully.
 *
 */


#ifndef AFMODULE_H_
#define AFMODULE_H_

#include <freetype/internal/ftobjs.h>
#include <freetype/ftmodapi.h>

#include "afhints.h"

#include "ft-hb.h"

FT_BEGIN_HEADER

  typedef struct  AF_HintsWorkspaceRec_
  {
    struct AF_HintsWorkspaceRec_*  next;
    FT_Bool                       in_use;
    AF_GlyphHintsRec               hints[1];
    AF_StyleMetrics                metrics;
    FT_ULong                       metrics_size;

  } AF_HintsWorkspaceRec, *AF_HintsWorkspace;



  /*
   * This is the `extended' FT_Module structure that holds the
   * autofitter's global data.
   */

  typedef struct  AF_ModuleRec_
  {
    FT_ModuleRec  root;

    FT_UInt       fallback_style;
    AF_Script     default_script;
    FT_Bool       no_stem_darkening;
    FT_Int        darken_params[8];

    /* Leased independently by glyph loading and nested metrics analysis. */
    AF_HintsWorkspace  workspaces;

#if defined( FT_CONFIG_OPTION_USE_HARFBUZZ )         && \
    defined( FT_CONFIG_OPTION_USE_HARFBUZZ_DYNAMIC )
    ft_hb_funcs_t*  hb_funcs;
#endif

  } AF_ModuleRec, *AF_Module;

  FT_LOCAL( FT_Error )
  af_module_acquire_workspace( AF_Module           module,
                               FT_ULong            metrics_size,
                               AF_HintsWorkspace  *aworkspace );

  FT_LOCAL( void )
  af_module_release_workspace( AF_HintsWorkspace  workspace,
                               FT_Error           error );



FT_DECLARE_AUTOHINTER_INTERFACE( af_autofitter_interface )
FT_DECLARE_MODULE( autofit_module_class )


FT_END_HEADER

#endif /* AFMODULE_H_ */


/* END */
