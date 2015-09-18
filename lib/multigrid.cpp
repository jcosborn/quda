#include <multigrid.h>
#include <qio_field.h>
#include <string.h>

namespace quda {  

  MG::MG(MGParam &param, TimeProfile &profile) 
    : Solver(param, profile), param(param), presmoother(0), postsmoother(0), profile_smoother("Smoother"), 
      coarse(0), fine(param.fine), param_coarse(0), param_presmooth(0), param_postsmooth(0), r(0), r_coarse(0), x_coarse(0), 
      diracCoarse(0), matCoarse(0) {

    // for reporting level 1 is the fine level but internally use level 0 for indexing
    sprintf(prefix,"MG level %d: ", param.level+1);
    setOutputPrefix(prefix);

    printfQuda("Creating level %d of %d levels\n", param.level+1, param.Nlevel);

    if (param.level >= QUDA_MAX_MG_LEVEL)
      errorQuda("Level=%d is greater than limit of multigrid recursion depth", param.level+1);

    // create the smoother for this level
    printfQuda("smoother has operator %s\n", typeid(param.matSmooth).name());

    param_presmooth = new MGParam(param, param.B, param.matResidual, param.matSmooth);

    param_presmooth->inv_type = param.smoother;
    if (param_presmooth->level < param.Nlevel) param_presmooth->inv_type_precondition = QUDA_GCR_INVERTER;
    param_presmooth->preserve_source = QUDA_PRESERVE_SOURCE_YES;
    param_presmooth->use_init_guess = QUDA_USE_INIT_GUESS_NO;
    param_presmooth->maxiter = param.nu_pre;
    param_presmooth->Nkrylov = 4;
    param_presmooth->inv_type_precondition = QUDA_INVALID_INVERTER;
    if (param.level==param.Nlevel-1) {
      param_presmooth->Nkrylov = 100;
      param_presmooth->maxiter = 1000;
      param_presmooth->tol = 1e-3;
      param_presmooth->preserve_source = QUDA_PRESERVE_SOURCE_NO;
      param_presmooth->delta = 1e-3;
    }
    presmoother = Solver::create(*param_presmooth, param_presmooth->matResidual,
				 param_presmooth->matSmooth, param_presmooth->matSmooth, profile_smoother);

    if (param.level < param.Nlevel-1) {

      //Create the post smoother
      param_postsmooth = new MGParam(param, param.B, param.matResidual, param.matSmooth);
      
      param_postsmooth->inv_type = param.smoother;
      if (param_postsmooth->level == 1) param_postsmooth->inv_type_precondition = QUDA_GCR_INVERTER;
      param_postsmooth->preserve_source = QUDA_PRESERVE_SOURCE_YES;
      param_postsmooth->use_init_guess = QUDA_USE_INIT_GUESS_YES;
      param_postsmooth->maxiter = param.nu_post;
      param_postsmooth->Nkrylov = 4;
      param_postsmooth->inv_type_precondition = QUDA_INVALID_INVERTER;
      postsmoother = Solver::create(*param_postsmooth, param_postsmooth->matResidual, 
				    param_postsmooth->matSmooth, param_postsmooth->matSmooth, profile_smoother);
    }

    // create residual vectors
    {
      ColorSpinorParam csParam(*(param.B[0]));
      csParam.create = QUDA_NULL_FIELD_CREATE;
      csParam.location = param.location;
      if (csParam.location==QUDA_CUDA_FIELD_LOCATION) {
	csParam.fieldOrder = (csParam.precision == QUDA_DOUBLE_PRECISION) ? QUDA_FLOAT2_FIELD_ORDER : QUDA_FLOAT4_FIELD_ORDER;
	csParam.setPrecision(csParam.precision);
	csParam.gammaBasis = QUDA_UKQCD_GAMMA_BASIS;
      }
      r = ColorSpinorField::Create(csParam);
    }

    // if not on the coarsest level, construct it
    if (param.level < param.Nlevel-1) {
      // create transfer operator
      printfQuda("start creating transfer operator\n");
      transfer = new Transfer(param.B, param.Nvec, param.geoBlockSize, param.spinBlockSize);
      printfQuda("end creating transfer operator\n");

      // create coarse residual vector
      r_coarse = param.B[0]->CreateCoarse(param.geoBlockSize, param.spinBlockSize, param.Nvec);

      // create coarse solution vector
      x_coarse = param.B[0]->CreateCoarse(param.geoBlockSize, param.spinBlockSize, param.Nvec);

      // create coarse grid operator

      // these need to be gpu fields with native ordering basis
      ColorSpinorParam csParam(*(param.B[0]));
      csParam.fieldOrder = (csParam.precision == QUDA_DOUBLE_PRECISION) ? QUDA_FLOAT2_FIELD_ORDER : QUDA_FLOAT4_FIELD_ORDER;
      csParam.setPrecision(csParam.precision);
      csParam.gammaBasis = QUDA_UKQCD_GAMMA_BASIS;
      csParam.create = QUDA_ZERO_FIELD_CREATE;

      printfQuda("creating coarse operator of type %s\n", typeid(matCoarse).name());

      DiracParam diracParam;
      diracParam.transfer = transfer;
      diracParam.dirac = const_cast<Dirac*>(param.matResidual.Expose());
      diracParam.kappa = param.matResidual.Expose()->Kappa();
      printfQuda("Kappa = %e\n", diracParam.kappa);
      diracCoarse = new DiracCoarse(diracParam);
      matCoarse = new DiracM(*diracCoarse);


      printfQuda("coarse operator of type %s created\n", typeid(matCoarse).name());

      // coarse null space vectors (dummy for now)
      printfQuda("Creating coarse null-space vectors\n");
      B_coarse = new std::vector<ColorSpinorField*>();
      B_coarse->resize(param.Nvec);

      for (int i=0; i<param.Nvec; i++) {
	(*B_coarse)[i] = param.B[0]->CreateCoarse(param.geoBlockSize, param.spinBlockSize, param.Nvec);
	blas::zero(*(*B_coarse)[i]);
	transfer->R(*(*B_coarse)[i], *(param.B[i]));
	#if 0
	if (param.level != 99) {
	  ColorSpinorParam csParam2(*(*B_coarse)[i]);
	  csParam2.create = QUDA_ZERO_FIELD_CREATE;
	  ColorSpinorField *tmp = ColorSpinorField::Create(csParam2);
	  for (int s=i; s<(*B_coarse)[i]->Nspin(); s+=2) {
	    for (int c=0; c<(*B_coarse)[i]->Ncolor(); c++) {
	      tmp->Source(QUDA_CONSTANT_SOURCE, 1, s, c);
	      //tmp->Source(QUDA_SINUSOIDAL_SOURCE, 3, 2);                                                                                       
	      //blas::xpy(*tmp,*(*B_coarse)[i]);
	    }
	  }
	  delete tmp;
          (*B_coarse)[i]->Source(QUDA_RANDOM_SOURCE);                                                                                      
          printfQuda("B_coarse[%d]\n", i);
          //for (int x=0; x<(*B_coarse)[i]->Volume(); x++) static_cast<cpuColorSpinorField*>((*B_coarse)[i])->PrintVector(x);
	}
	#endif

      }

      // create the next multigrid level
      printfQuda("Creating next multigrid level\n");
      param_coarse = new MGParam(param, *B_coarse, *matCoarse, *matCoarse, param.level+1);
      param_coarse->fine = this;
      param_coarse->delta = 1e-20;

      coarse = new MG(*param_coarse, profile);
      setOutputPrefix(prefix); // restore since we just popped back from coarse grid
    }

    printfQuda("setup completed\n");

    // now we can run through the verificaion
    if (param.level < param.Nlevel-1) verify();  
  }

  MG::~MG() {
    if (param.level < param.Nlevel-1) {
      if (B_coarse) for (int i=0; i<param.Nvec; i++) delete (*B_coarse)[i];
      if (coarse) delete coarse;
      if (transfer) delete transfer;
      if (matCoarse) delete matCoarse;
      if (diracCoarse) delete diracCoarse;
      if (postsmoother) delete postsmoother;
    }
    if (presmoother) delete presmoother;

    if (r) delete r;
    if (r_coarse) delete r_coarse;
    if (x_coarse) delete x_coarse;

    if (param_coarse) delete param_coarse;
    if (param_presmooth) delete param_presmooth;
    if (param_postsmooth) delete param_postsmooth;

    if (getVerbosity() >= QUDA_VERBOSE) profile_smoother.Print();
  }

  /**
     Verification that the constructed multigrid operator is valid
   */
  void MG::verify() {
    // temporary fields used for verification
    ColorSpinorParam csParam(*(param.B[0]));
    csParam.create = QUDA_NULL_FIELD_CREATE;
    cpuColorSpinorField *tmp1 = new cpuColorSpinorField(csParam);
    cpuColorSpinorField *tmp2 = new cpuColorSpinorField(csParam);

    printfQuda("\n");
    printfQuda("Checking 0 = (1 - P^\\dagger P) v_k for %d vectors\n", param.Nvec);

    for (int i=0; i<param.Nvec; i++) {
      transfer->R(*r_coarse, *param.B[i]);

      transfer->P(*tmp2, *r_coarse);
      printfQuda("Vector %d: norms %e %e (coarse tmp %e) ", i, blas::norm2(*param.B[i]), blas::norm2(*tmp2), blas::norm2(*r_coarse));
      printfQuda("deviation = %e\n", blas::xmyNorm(*(param.B[i]), *tmp2));
    }
#if 0
    printfQuda("Checking 1 > || (1 - D P (P^\\dagger D P) P^\\dagger v_k || / || v_k || for %d vectors\n", 
	       param.Nvec);

    for (int i=0; i<param.Nvec; i++) {
      transfer->R(*r_coarse, *(param.B[i]));
      (*coarse)(*x_coarse, *r_coarse);
      transfer->P(*tmp2, *x_coarse);
      param.matResidual(*tmp1,*tmp2);
      printfQuda("Vector %d: norms %e %e ", i, blas::norm2(*param.B[i]), blas::norm2(*tmp1));
      printfQuda("relative residual = %e\n", sqrt(blas::xmyNorm(*(param.B[i]), *tmp1) / blas::norm2(*param.B[i])) );
    }
#endif

    printfQuda("\n");
    printfQuda("Checking 0 = (1 - P^\\dagger P) eta_c\n");
    x_coarse->Source(QUDA_RANDOM_SOURCE);
    transfer->P(*tmp2, *x_coarse);
    transfer->R(*r_coarse, *tmp2);
    printfQuda("Vector norms %e %e (fine tmp %e) ", blas::norm2(*x_coarse), blas::norm2(*r_coarse), blas::norm2(*tmp2));
    printfQuda("deviation = %e\n", blas::xmyNorm(*x_coarse, *r_coarse));

    printfQuda("\n");
    printfQuda("Comparing native coarse operator to emulated operator\n");
    ColorSpinorField *tmp_coarse = param.B[0]->CreateCoarse(param.geoBlockSize, param.spinBlockSize, param.Nvec);
    blas::zero(*tmp_coarse);
    blas::zero(*r_coarse);
    #if 1
    tmp_coarse->Source(QUDA_RANDOM_SOURCE);
    #else
    for(int s = 0; s < tmp_coarse->Nspin(); s++)
      for(int c=0; c < tmp_coarse->Ncolor(); c++) 
	tmp_coarse->Source(QUDA_POINT_SOURCE,0,s,c);
    #endif
    transfer->P(*tmp1, *tmp_coarse);
    param.matResidual(*tmp2,*tmp1);	
    transfer->R(*x_coarse, *tmp2);
    param_coarse->matResidual(*r_coarse, *tmp_coarse);
    #if 0
    if(param.level == 0) {
      printfQuda("x_coarse\n");
      //static_cast<cpuColorSpinorField*>(x_coarse)->PrintVector(0);
      for (int x=0; x<x_coarse->Volume(); x++) if(x<2) static_cast<cpuColorSpinorField*>(x_coarse)->PrintVector(x);
      printfQuda("r_coarse\n");
      for (int x=0; x<r_coarse->Volume(); x++) if(x<2) static_cast<cpuColorSpinorField*>(r_coarse)->PrintVector(x);
      //static_cast<cpuColorSpinorField*>(r_coarse)->PrintVector(0);
    }
    #endif
    printfQuda("Vector norms Emulated=%e Native=%e ", blas::norm2(*x_coarse), blas::norm2(*r_coarse));
    printfQuda("deviation = %e\n\n", blas::xmyNorm(*x_coarse, *r_coarse));

    delete tmp1;
    delete tmp2;
    delete tmp_coarse;
  }

  void setTransferGPU(bool use_gpu);

  void MG::operator()(ColorSpinorField &x, ColorSpinorField &b) {
    setOutputPrefix(prefix);

    if (getVerbosity() >= QUDA_VERBOSE)
      printfQuda("entering V-cycle with x2=%e, r2=%e\n", blas::norm2(x), blas::norm2(b));

    if (param.level < param.Nlevel-1) {
      
      // do the pre smoothing
      printfQuda("pre-smoothing b2=%e\n", blas::norm2(b));
      (*presmoother)(x, b);

      printfQuda("done smoother %d %d %d\n", r->Location(), x.Location(), b.Location());

      // FIXME - residual computation should be in the previous smoother
      param.matResidual(*r, x);
      double r2 = blas::xmyNorm(b, *r);

      // restrict to the coarse grid
      if (getVerbosity() >= QUDA_VERBOSE) printfQuda("restriction\n");

      transfer->R(*r_coarse, *r);

      /*printf("now doing CPU\n");
      setTransferGPU(false);
      transfer->R(*x_coarse, *r);*/

      if (getVerbosity() >= QUDA_VERBOSE) 
	printfQuda("after pre-smoothing x2 = %e, r2 = %e, r_coarse2 = %e\n", 
		   blas::norm2(x), r2, blas::norm2(*r_coarse));

      // recurse to the next lower level
      (*coarse)(*x_coarse, *r_coarse);
      setOutputPrefix(prefix); // restore prefix after return from coarse grid

      if (getVerbosity() >= QUDA_VERBOSE) 
	printfQuda("after coarse solve x_coarse2 = %e r_coarse2 = %e\n", 
		   blas::norm2(*x_coarse), blas::norm2(*r_coarse)); 

      // prolongate back to this grid
      transfer->P(*r, *x_coarse); // repurpose residual storage
      // FIXME - sum should be done inside the transfer operator
      blas::xpy(*r, x); // sum to solution

      if (getVerbosity() >= QUDA_VERBOSE) {
	printfQuda("Prolongated coarse solution y2 = %e\n", blas::norm2(*r)); 
	printfQuda("after coarse-grid correction x2 = %e, r2 = %e\n", 
		   blas::norm2(x), blas::norm2(*r));
      }

      // do the post smoothing
      (*postsmoother)(x,b);
    } else { // do the coarse grid solve
      (*presmoother)(x, b);
    }

    if (getVerbosity() >= QUDA_VERBOSE) {
      param.matResidual(*r, x);
      double r2 = blas::xmyNorm(b, *r);
      printfQuda("leaving V-cycle with x2=%e, r2=%e\n", blas::norm2(x), r2);
    }

    setOutputPrefix("");
  }

  //supports seperate reading or single file read
  void loadVectors(std::vector<ColorSpinorField*> &B) {
    const int Nvec = B.size();
    printfQuda("Start loading %d vectors from %s\n", Nvec, vecfile);


    void **V = new void*[Nvec];
    for (int i=0; i<Nvec; i++) { 
      V[i] = B[i]->V();
      if (V[i] == NULL) {
	printf("Could not allocate V[%d]\n", i);      
      }
    }
    
    if (strcmp(vecfile,"")!=0) {
#if 0
      read_spinor_field(vecfile, &V[0], B[0]->Precision(), B[0]->X(), 
			B[0]->Ncolor(), B[0]->Nspin(), Nvec, 0,  (char**)0);
#else 
      for (int i=0; i<Nvec; i++) {
	char filename[256];
	sprintf(filename, "%s.%d", vecfile, i);
	printf("Reading vector %d from file %s\n", i, filename);
	read_spinor_field(filename, &V[i], B[i]->Precision(), B[i]->X(), 
			  B[i]->Ncolor(), B[i]->Nspin(), 1, 0,  (char**)0);
      }
#endif
    } else {
      printfQuda("Using %d constant nullvectors\n", Nvec);
      //errorQuda("No nullspace file defined");

      for (int i = 0; i < (Nvec < 2 ? Nvec : 2); i++) {
	blas::zero(*B[i]);
#if 1
	ColorSpinorParam csParam(*B[i]);
	csParam.create = QUDA_ZERO_FIELD_CREATE;
	ColorSpinorField *tmp = ColorSpinorField::Create(csParam);
	for (int s=i; s<4; s+=2) {
	  for (int c=0; c<B[i]->Ncolor(); c++) {
	    tmp->Source(QUDA_CONSTANT_SOURCE, 1, s, c);
	    //tmp->Source(QUDA_SINUSOIDAL_SOURCE, 3, 2);
	    blas::xpy(*tmp,*B[i]);
	  }
	}
	delete tmp;
#else
	printfQuda("Using random source for nullvector = %d\n",i);
	B[i]->Source(QUDA_RANDOM_SOURCE);
#endif
	//printfQuda("B[%d]\n",i);
	//for (int x=0; x<B[i]->Volume(); x++) static_cast<cpuColorSpinorField*>(B[i])->PrintVector(x);
      }

      for (int i=2; i<Nvec; i++) B[i] -> Source(QUDA_RANDOM_SOURCE);
    }

    printfQuda("Done loading vectors\n");
  }

}
