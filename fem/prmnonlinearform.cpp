// Copyright (c) 2010-2020, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.


#include "fem.hpp"


namespace mfem
{

ParametricBNLForm::ParametricBNLForm(): fes(0), prmfes(0), BlockGrad(nullptr)
{
    height = 0;
    width  = 0;

    prmheight = 0;
    prmwidth = 0;
}

ParametricBNLForm::ParametricBNLForm(Array<FiniteElementSpace *> &f, Array<FiniteElementSpace *> &pf):
    fes(0),prmfes(0), BlockGrad(nullptr)
{
    SetSpaces(f,pf);
}

void ParametricBNLForm::SetSpaces(Array<FiniteElementSpace *> &f, Array<FiniteElementSpace *> &prmf)
{
    delete BlockGrad;
    BlockGrad = nullptr;
    for (int i=0; i<Grads.NumRows(); ++i)
    {
       for (int j=0; j<Grads.NumCols(); ++j)
       {
          delete Grads(i,j);
          delete cGrads(i,j);
       }
    }
    for (int i = 0; i < ess_tdofs.Size(); ++i)
    {
       delete ess_tdofs[i];
    }
    for (int i = 0; i < prmess_tdofs.Size(); ++i)
    {
        delete prmess_tdofs[i];
    }

    height = 0;
    width = 0;
    //set state feses
    f.Copy(fes);
    //set the block sizes
    block_offsets.SetSize(f.Size() + 1);
    block_trueOffsets.SetSize(f.Size() + 1);
    block_offsets[0] = 0;
    block_trueOffsets[0] = 0;
    for (int i=0; i<fes.Size(); ++i)
    {
       block_offsets[i+1] = fes[i]->GetVSize();
       block_trueOffsets[i+1] = fes[i]->GetTrueVSize();
    }
    block_offsets.PartialSum();
    block_trueOffsets.PartialSum();


    //set design/parametric feses
    prmf.Copy(prmfes);
    prmblock_offsets.SetSize(prmf.Size() + 1);
    prmblock_trueOffsets.SetSize(prmf.Size() + 1);
    prmblock_offsets[0] = 0;
    prmblock_trueOffsets[0] = 0;
    for (int i=0; i<prmfes.Size(); ++i)
    {
       prmblock_offsets[i+1] = prmfes[i]->GetVSize();
       prmblock_trueOffsets[i+1] = prmfes[i]->GetTrueVSize();
    }
    prmblock_offsets.PartialSum();
    prmblock_trueOffsets.PartialSum();

    //set the size of the operator
    height = block_trueOffsets[fes.Size()];
    width = block_trueOffsets[fes.Size()];

    Grads.SetSize(fes.Size(), fes.Size());
    Grads = NULL;

    cGrads.SetSize(fes.Size(), fes.Size());
    cGrads = NULL;

    P.SetSize(fes.Size());
    cP.SetSize(fes.Size());
    ess_tdofs.SetSize(fes.Size());

    for (int s = 0; s < fes.Size(); ++s)
    {
       // Retrieve prolongation matrix for each FE space
       P[s] = fes[s]->GetProlongationMatrix();
       cP[s] = dynamic_cast<const SparseMatrix *>(P[s]);

       // If the P Operator exists and its type is not SparseMatrix, this
       // indicates the Operator is part of parallel run.
       if (P[s] && !cP[s])
       {
          is_serial = false;
       }

       // If the P Operator exists and its type is SparseMatrix, this indicates
       // the Operator is serial but needs prolongation on assembly.
       if (cP[s])
       {
          needs_prolongation = true;
       }

       ess_tdofs[s] = new Array<int>;
    }

    //set the size of the design/parametric part
    prmheight = prmblock_trueOffsets[prmfes.Size()];
    prmwidth = prmblock_trueOffsets[prmfes.Size()];

    Pprm.SetSize(prmfes.Size());
    cPprm.SetSize(prmfes.Size());
    prmess_tdofs.SetSize(prmfes.Size());

    for (int s = 0; s < prmfes.Size(); ++s)
    {
       // Retrieve prolongation matrix for each FE space
       Pprm[s] = prmfes[s]->GetProlongationMatrix();
       cPprm[s] = dynamic_cast<const SparseMatrix *>(Pprm[s]);

       // If the P Operator exists and its type is SparseMatrix, this indicates
       // the Operator is serial but needs prolongation on assembly.
       if (cPprm[s])
       {
          prmneeds_prolongation = true;
       }

       prmess_tdofs[s] = new Array<int>;
    }

    xsv.Update(block_offsets);
    adv.Update(block_offsets);
    xdv.Update(prmblock_offsets);

}


void ParametricBNLForm::AddBdrFaceIntegrator(ParametricBNLFormIntegrator *nlfi,
                                                            Array<int> &bdr_marker)
{
    bfnfi.Append(nlfi);
    bfnfi_marker.Append(&bdr_marker);
}


double ParametricBNLForm::GetEnergyBlocked(const BlockVector &bx, const BlockVector &dx) const
{
   Array<Array<int> *> vdofs(fes.Size());
   Array<Vector *> el_x(fes.Size());
   Array<const Vector *> el_x_const(fes.Size());
   Array<const FiniteElement *> fe(fes.Size());
   ElementTransformation *T;

   Array<Array<int> *> prmvdofs(prmfes.Size());
   Array<Vector *> prmel_x(prmfes.Size());
   Array<const Vector *> prmel_x_const(prmfes.Size());
   Array<const FiniteElement *> prmfe(prmfes.Size());



   double energy = 0.0;

   for (int i=0; i<fes.Size(); ++i)
   {
      el_x_const[i] = el_x[i] = new Vector();
      vdofs[i] = new Array<int>;
   }

   for (int i=0; i<prmfes.Size(); ++i)
   {
      prmel_x_const[i] = prmel_x[i] = new Vector();
      prmvdofs[i] = new Array<int>;
   }


   if (dnfi.Size()){
      for (int i = 0; i < fes[0]->GetNE(); ++i)
      {
         T = fes[0]->GetElementTransformation(i);
         for (int s=0; s<fes.Size(); ++s)
         {
            fe[s] = fes[s]->GetFE(i);
            fes[s]->GetElementVDofs(i, *vdofs[s]);
            bx.GetBlock(s).GetSubVector(*vdofs[s], *el_x[s]);
         }

         for (int s=0; s<prmfes.Size(); ++s)
         {
            prmfe[s] = prmfes[s]->GetFE(i);
            prmfes[s]->GetElementVDofs(i, *prmvdofs[s]);
            dx.GetBlock(s).GetSubVector(*prmvdofs[s], *prmel_x[s]);
         }



         for (int k = 0; k < dnfi.Size(); ++k)
         {
            energy += dnfi[k]->GetElementEnergy(fe, prmfe, *T, el_x_const, prmel_x_const);
         }
      }
   }

   // free the allocated memory
   for (int i = 0; i < fes.Size(); ++i)
   {
      delete el_x[i];
      delete vdofs[i];
   }

   for (int i = 0; i < prmfes.Size(); ++i)
   {
      delete prmel_x[i];
      delete prmvdofs[i];
   }

   if (fnfi.Size())
   {
      MFEM_ABORT("TODO: add energy contribution from interior face terms");
   }

   if (bfnfi.Size())
   {
      MFEM_ABORT("TODO: add energy contribution from boundary face terms");
   }

   return energy;
}

void ParametricBNLForm::SetStateFields(const Vector &xv) const
{
    BlockVector bx(xv.GetData(), block_trueOffsets);
    if(needs_prolongation)
    {
        for (int s = 0; s < fes.Size(); s++)
        {
           P[s]->Mult(bx.GetBlock(s), xsv.GetBlock(s));
        }
    }
    else
    {
        xsv=bx;
    }
}


void ParametricBNLForm::SetAdjointFields(const Vector &av) const
{
    BlockVector bx(av.GetData(), block_trueOffsets);
    if(needs_prolongation)
    {
        for (int s = 0; s < fes.Size(); s++)
        {
           P[s]->Mult(bx.GetBlock(s), adv.GetBlock(s));
        }
    }
    else
    {
        adv=bx;
    }

}

void ParametricBNLForm::SetPrmFields(const Vector &dv) const
{
    BlockVector bx(dv.GetData(), prmblock_trueOffsets);
    if(prmneeds_prolongation)
    {
        for (int s = 0; s < prmfes.Size(); s++)
        {
           Pprm[s]->Mult(bx.GetBlock(s), xdv.GetBlock(s));
        }
    }
    else
    {
        xdv=bx;
    }
}

double ParametricBNLForm::GetEnergy(const Vector &x) const
{
   xs.Update(x.GetData(),block_offsets);
   return GetEnergyBlocked(xs,xdv);
}

void ParametricBNLForm::SetEssentialBC(const Array<Array<int> *> &bdr_attr_is_ess,
                                           Array<Vector *> &rhs)
{
    for (int s = 0; s < fes.Size(); ++s)
    {
       ess_tdofs[s]->SetSize(ess_tdofs.Size());

       fes[s]->GetEssentialTrueDofs(*bdr_attr_is_ess[s], *ess_tdofs[s]);

       if (rhs[s])
       {
          rhs[s]->SetSubVector(*ess_tdofs[s], 0.0);
       }
    }
}

void ParametricBNLForm::SetPrmEssentialBC(const Array<Array<int> *> &bdr_attr_is_ess,
                                              Array<Vector *> &rhs)
{
    for (int s = 0; s < prmfes.Size(); ++s)
    {
       prmess_tdofs[s]->SetSize(prmess_tdofs.Size());

       prmfes[s]->GetEssentialTrueDofs(*bdr_attr_is_ess[s], *prmess_tdofs[s]);

       if (rhs[s])
       {
          rhs[s]->SetSubVector(*prmess_tdofs[s], 0.0);
       }
    }
}

void ParametricBNLForm::MultPrmBlocked(const BlockVector &bx,
                                           const BlockVector &ax,
                                           const BlockVector &dx,
                                           BlockVector &dy) const
{
    // state fields
    Array<Array<int> *>vdofs(fes.Size());
    Array<Array<int> *>vdofs2(fes.Size());
    Array<Vector *> el_x(fes.Size());
    Array<const Vector *> el_x_const(fes.Size());
    //Array<Vector *> el_y(fes.Size());
    Array<const FiniteElement *> fe(fes.Size());
    Array<const FiniteElement *> fe2(fes.Size());

    ElementTransformation *T;

    // adjoint fields
    Array<Vector *> ael_x(fes.Size());
    Array<const Vector *> ael_x_const(fes.Size());

    // parametric fields
    Array<Array<int> *>prmvdofs(prmfes.Size());
    Array<Array<int> *>prmvdofs2(prmfes.Size());
    Array<Vector *> prmel_x(prmfes.Size());
    Array<const Vector *> prmel_x_const(prmfes.Size());
    Array<Vector *> prmel_y(prmfes.Size());
    Array<const FiniteElement *> prmfe(prmfes.Size());
    Array<const FiniteElement *> prmfe2(prmfes.Size());

    dy = 0.0;

    for (int s=0; s<fes.Size(); ++s)
    {
       el_x_const[s] = el_x[s] = new Vector();
       //el_y[s] = new Vector();
       vdofs[s] = new Array<int>;
       vdofs2[s] = new Array<int>;

       ael_x_const[s] = ael_x[s] = new Vector();
    }

    for (int s=0; s<prmfes.Size(); ++s)
    {
       prmel_x_const[s] = prmel_x[s] = new Vector();
       prmel_y[s] = new Vector();
       prmvdofs[s] = new Array<int>;
       prmvdofs2[s] = new Array<int>;
    }

    if (dnfi.Size())
    {
       for (int i = 0; i < fes[0]->GetNE(); ++i)
       {
          T = fes[0]->GetElementTransformation(i);
          for (int s = 0; s < fes.Size(); ++s)
          {
             fes[s]->GetElementVDofs(i, *(vdofs[s]));
             fe[s] = fes[s]->GetFE(i);
             bx.GetBlock(s).GetSubVector(*(vdofs[s]), *el_x[s]);
             ax.GetBlock(s).GetSubVector(*(vdofs[s]), *ael_x[s]);
          }

          for (int s = 0; s < prmfes.Size(); ++s)
          {
             prmfes[s]->GetElementVDofs(i, *(prmvdofs[s]));
             prmfe[s] = prmfes[s]->GetFE(i);
             dx.GetBlock(s).GetSubVector(*(prmvdofs[s]), *prmel_x[s]);
          }


          for (int k = 0; k < dnfi.Size(); ++k)
          {
             dnfi[k]->AssemblePrmElementVector(fe,prmfe, *T,
                                               el_x_const, ael_x_const, prmel_x_const,
                                               prmel_y);

             for (int s=0; s<prmfes.Size(); ++s)
             {
                if (prmel_y[s]->Size() == 0) { continue; }
                dy.GetBlock(s).AddElementVector(*(prmvdofs[s]), *prmel_y[s]);
             }
          }
       }
    }

    if (fnfi.Size())
    {
       Mesh *mesh = fes[0]->GetMesh();
       FaceElementTransformations *tr;

       for (int i = 0; i < mesh->GetNumFaces(); ++i)
       {
          tr = mesh->GetInteriorFaceTransformations(i);
          if (tr != NULL)
          {
             for (int s=0; s<fes.Size(); ++s)
             {
                fe[s] = fes[s]->GetFE(tr->Elem1No);
                fe2[s] = fes[s]->GetFE(tr->Elem2No);

                fes[s]->GetElementVDofs(tr->Elem1No, *(vdofs[s]));
                fes[s]->GetElementVDofs(tr->Elem2No, *(vdofs2[s]));

                vdofs[s]->Append(*(vdofs2[s]));

                bx.GetBlock(s).GetSubVector(*(vdofs[s]), *el_x[s]);
                ax.GetBlock(s).GetSubVector(*(vdofs[s]), *ael_x[s]);
             }

             for (int s=0; s<prmfes.Size(); ++s)
             {
                prmfe[s] = prmfes[s]->GetFE(tr->Elem1No);
                prmfe2[s] = prmfes[s]->GetFE(tr->Elem2No);

                prmfes[s]->GetElementVDofs(tr->Elem1No, *(prmvdofs[s]));
                prmfes[s]->GetElementVDofs(tr->Elem2No, *(prmvdofs2[s]));

                prmvdofs[s]->Append(*(prmvdofs2[s]));

                dx.GetBlock(s).GetSubVector(*(prmvdofs[s]), *prmel_x[s]);
             }

             for (int k = 0; k < fnfi.Size(); ++k)
             {

                fnfi[k]->AssemblePrmFaceVector(fe, fe2, prmfe, prmfe2, *tr,
                                               el_x_const, ael_x_const, prmel_x_const, prmel_y);

                for (int s=0; s<prmfes.Size(); ++s)
                {
                   if (prmel_y[s]->Size() == 0) { continue; }
                   dy.GetBlock(s).AddElementVector(*(prmvdofs[s]), *prmel_y[s]);
                }
             }
          }
       }
    }

    if (bfnfi.Size())
    {
       Mesh *mesh = fes[0]->GetMesh();
       FaceElementTransformations *tr;
       // Which boundary attributes need to be processed?
       Array<int> bdr_attr_marker(mesh->bdr_attributes.Size() ?
                                  mesh->bdr_attributes.Max() : 0);
       bdr_attr_marker = 0;
       for (int k = 0; k < bfnfi.Size(); ++k)
       {
          if (bfnfi_marker[k] == NULL)
          {
             bdr_attr_marker = 1;
             break;
          }
          Array<int> &bdr_marker = *bfnfi_marker[k];
          MFEM_ASSERT(bdr_marker.Size() == bdr_attr_marker.Size(),
                      "invalid boundary marker for boundary face integrator #"
                      << k << ", counting from zero");
          for (int i = 0; i < bdr_attr_marker.Size(); ++i)
          {
             bdr_attr_marker[i] |= bdr_marker[i];
          }
       }

       for (int i = 0; i < mesh->GetNBE(); ++i)
       {
          const int bdr_attr = mesh->GetBdrAttribute(i);
          if (bdr_attr_marker[bdr_attr-1] == 0) { continue; }

          tr = mesh->GetBdrFaceTransformations(i);
          if (tr != NULL)
          {
             for (int s=0; s<fes.Size(); ++s)
             {
                fe[s] = fes[s]->GetFE(tr->Elem1No);
                fe2[s] = fes[s]->GetFE(tr->Elem1No);

                fes[s]->GetElementVDofs(tr->Elem1No, *(vdofs[s]));
                bx.GetBlock(s).GetSubVector(*(vdofs[s]), *el_x[s]);
                ax.GetBlock(s).GetSubVector(*(vdofs[s]), *ael_x[s]);
             }

             for (int s=0; s<prmfes.Size(); ++s)
             {
                prmfe[s] = prmfes[s]->GetFE(tr->Elem1No);
                prmfe2[s] = prmfes[s]->GetFE(tr->Elem1No);

                prmfes[s]->GetElementVDofs(tr->Elem1No, *(prmvdofs[s]));
                dx.GetBlock(s).GetSubVector(*(prmvdofs[s]), *prmel_x[s]);
             }


             for (int k = 0; k < bfnfi.Size(); ++k)
             {
                if (bfnfi_marker[k] &&
                    (*bfnfi_marker[k])[bdr_attr-1] == 0) { continue; }

                bfnfi[k]->AssemblePrmFaceVector(fe, fe2, prmfe, prmfe2, *tr,
                                                el_x_const, ael_x_const, prmel_x_const, prmel_y);

                for (int s=0; s<prmfes.Size(); ++s)
                {
                   if (prmel_y[s]->Size() == 0) { continue; }
                   dy.GetBlock(s).AddElementVector(*(prmvdofs[s]), *prmel_y[s]);
                }
             }
          }
       }
    }

    for (int s=0; s<fes.Size(); ++s)
    {
       delete vdofs2[s];
       delete vdofs[s];
       delete el_x[s];
       delete ael_x[s];
    }

    for (int s=0; s<prmfes.Size(); ++s)
    {
       delete prmvdofs2[s];
       delete prmvdofs[s];
       delete prmel_y[s];
       delete prmel_x[s];
    }

}

void ParametricBNLForm::MultBlocked(const BlockVector &bx,
                                        const BlockVector &dx,
                                        BlockVector &by) const
{

    Array<Array<int> *>vdofs(fes.Size());
    Array<Array<int> *>vdofs2(fes.Size());
    Array<Vector *> el_x(fes.Size());
    Array<const Vector *> el_x_const(fes.Size());
    Array<Vector *> el_y(fes.Size());
    Array<const FiniteElement *> fe(fes.Size());
    Array<const FiniteElement *> fe2(fes.Size());

    ElementTransformation *T;

    Array<Array<int> *>prmvdofs(prmfes.Size());
    Array<Array<int> *>prmvdofs2(prmfes.Size());
    Array<Vector *> prmel_x(prmfes.Size());
    Array<const Vector *> prmel_x_const(prmfes.Size());
    //Array<Vector *> prmel_y(fes.Size());
    Array<const FiniteElement *> prmfe(prmfes.Size());
    Array<const FiniteElement *> prmfe2(prmfes.Size());



    by = 0.0;
    for (int s=0; s<fes.Size(); ++s)
    {
       el_x_const[s] = el_x[s] = new Vector();
       el_y[s] = new Vector();
       vdofs[s] = new Array<int>;
       vdofs2[s] = new Array<int>;
    }

    for (int s=0; s<prmfes.Size(); ++s)
    {
       prmel_x_const[s] = prmel_x[s] = new Vector();
       prmvdofs[s] = new Array<int>;
       prmvdofs2[s] = new Array<int>;
    }

    if (dnfi.Size())
    {
       for (int i = 0; i < fes[0]->GetNE(); ++i)
       {
          T = fes[0]->GetElementTransformation(i);
          for (int s = 0; s < fes.Size(); ++s)
          {
             fes[s]->GetElementVDofs(i, *(vdofs[s]));
             fe[s] = fes[s]->GetFE(i);
             bx.GetBlock(s).GetSubVector(*(vdofs[s]), *el_x[s]);
          }

          for (int s = 0; s < prmfes.Size(); ++s)
          {
             prmfes[s]->GetElementVDofs(i, *(prmvdofs[s]));
             prmfe[s] = prmfes[s]->GetFE(i);
             dx.GetBlock(s).GetSubVector(*(prmvdofs[s]), *prmel_x[s]);
          }


          for (int k = 0; k < dnfi.Size(); ++k)
          {
             dnfi[k]->AssembleElementVector(fe,prmfe, *T,
                                            el_x_const, prmel_x_const, el_y);

             for (int s=0; s<fes.Size(); ++s)
             {
                if (el_y[s]->Size() == 0) { continue; }
                by.GetBlock(s).AddElementVector(*(vdofs[s]), *el_y[s]);
             }
          }
       }
    }

    if (fnfi.Size())
    {
       Mesh *mesh = fes[0]->GetMesh();
       FaceElementTransformations *tr;

       for (int i = 0; i < mesh->GetNumFaces(); ++i)
       {
          tr = mesh->GetInteriorFaceTransformations(i);
          if (tr != NULL)
          {
             for (int s=0; s<fes.Size(); ++s)
             {
                fe[s] = fes[s]->GetFE(tr->Elem1No);
                fe2[s] = fes[s]->GetFE(tr->Elem2No);

                fes[s]->GetElementVDofs(tr->Elem1No, *(vdofs[s]));
                fes[s]->GetElementVDofs(tr->Elem2No, *(vdofs2[s]));

                vdofs[s]->Append(*(vdofs2[s]));

                bx.GetBlock(s).GetSubVector(*(vdofs[s]), *el_x[s]);
             }

             for (int s=0; s<prmfes.Size(); ++s)
             {
                prmfe[s] = prmfes[s]->GetFE(tr->Elem1No);
                prmfe2[s] = prmfes[s]->GetFE(tr->Elem2No);

                prmfes[s]->GetElementVDofs(tr->Elem1No, *(prmvdofs[s]));
                prmfes[s]->GetElementVDofs(tr->Elem2No, *(prmvdofs2[s]));

                prmvdofs[s]->Append(*(prmvdofs2[s]));

                dx.GetBlock(s).GetSubVector(*(prmvdofs[s]), *prmel_x[s]);
             }

             for (int k = 0; k < fnfi.Size(); ++k)
             {

                fnfi[k]->AssembleFaceVector(fe, fe2, prmfe, prmfe2, *tr, el_x_const, prmel_x_const, el_y);

                for (int s=0; s<fes.Size(); ++s)
                {
                   if (el_y[s]->Size() == 0) { continue; }
                   by.GetBlock(s).AddElementVector(*(vdofs[s]), *el_y[s]);
                }
             }
          }
       }
    }

    if (bfnfi.Size())
    {
       Mesh *mesh = fes[0]->GetMesh();
       FaceElementTransformations *tr;
       // Which boundary attributes need to be processed?
       Array<int> bdr_attr_marker(mesh->bdr_attributes.Size() ?
                                  mesh->bdr_attributes.Max() : 0);
       bdr_attr_marker = 0;
       for (int k = 0; k < bfnfi.Size(); ++k)
       {
          if (bfnfi_marker[k] == NULL)
          {
             bdr_attr_marker = 1;
             break;
          }
          Array<int> &bdr_marker = *bfnfi_marker[k];
          MFEM_ASSERT(bdr_marker.Size() == bdr_attr_marker.Size(),
                      "invalid boundary marker for boundary face integrator #"
                      << k << ", counting from zero");
          for (int i = 0; i < bdr_attr_marker.Size(); ++i)
          {
             bdr_attr_marker[i] |= bdr_marker[i];
          }
       }

       for (int i = 0; i < mesh->GetNBE(); ++i)
       {
          const int bdr_attr = mesh->GetBdrAttribute(i);
          if (bdr_attr_marker[bdr_attr-1] == 0) { continue; }

          tr = mesh->GetBdrFaceTransformations(i);
          if (tr != NULL)
          {
             for (int s=0; s<fes.Size(); ++s)
             {
                fe[s] = fes[s]->GetFE(tr->Elem1No);
                fe2[s] = fes[s]->GetFE(tr->Elem1No);

                fes[s]->GetElementVDofs(tr->Elem1No, *(vdofs[s]));
                bx.GetBlock(s).GetSubVector(*(vdofs[s]), *el_x[s]);
             }

             for (int s=0; s<prmfes.Size(); ++s)
             {
                prmfe[s] = prmfes[s]->GetFE(tr->Elem1No);
                prmfe2[s] = prmfes[s]->GetFE(tr->Elem1No);

                prmfes[s]->GetElementVDofs(tr->Elem1No, *(prmvdofs[s]));
                dx.GetBlock(s).GetSubVector(*(prmvdofs[s]), *prmel_x[s]);
             }


             for (int k = 0; k < bfnfi.Size(); ++k)
             {
                if (bfnfi_marker[k] &&
                    (*bfnfi_marker[k])[bdr_attr-1] == 0) { continue; }

                bfnfi[k]->AssembleFaceVector(fe, fe2, prmfe, prmfe2, *tr, el_x_const, prmel_x_const, el_y);

                for (int s=0; s<fes.Size(); ++s)
                {
                   if (el_y[s]->Size() == 0) { continue; }
                   by.GetBlock(s).AddElementVector(*(vdofs[s]), *el_y[s]);
                }
             }
          }
       }
    }

    for (int s=0; s<fes.Size(); ++s)
    {
       delete vdofs2[s];
       delete vdofs[s];
       delete el_y[s];
       delete el_x[s];
    }

    for (int s=0; s<prmfes.Size(); ++s)
    {
       delete prmvdofs2[s];
       delete prmvdofs[s];
       //delete prmel_y[s];
       delete prmel_x[s];
    }

}

const BlockVector &ParametricBNLForm::Prolongate(const BlockVector &bx) const
{
   MFEM_VERIFY(bx.Size() == Width(), "invalid input BlockVector size");

   if (needs_prolongation)
   {
      aux1.Update(block_offsets);
      for (int s = 0; s < fes.Size(); s++)
      {
         P[s]->Mult(bx.GetBlock(s), aux1.GetBlock(s));
      }
      return aux1;
   }
   return bx;
}

const BlockVector &ParametricBNLForm::PrmProlongate(const BlockVector &bx) const
{
   MFEM_VERIFY(bx.Size() == prmwidth, "invalid input BlockVector size");

   if (prmneeds_prolongation)
   {
      prmaux1.Update(prmblock_offsets);
      for (int s = 0; s < prmfes.Size(); s++)
      {
         Pprm[s]->Mult(bx.GetBlock(s), prmaux1.GetBlock(s));
      }
      return prmaux1;
   }
   return bx;
}


void ParametricBNLForm::PrmMult(const Vector &x, Vector &y) const
{
    BlockVector bx(x.GetData(), prmblock_trueOffsets);
    BlockVector by(y.GetData(), prmblock_trueOffsets);

    const BlockVector &pbx = PrmProlongate(bx);

    if(prmneeds_prolongation)
    {
        prmaux2.Update(prmblock_offsets);
    }
    BlockVector &pby = prmneeds_prolongation ? prmaux2 : by;

    xs.Update(pbx.GetData(), prmblock_offsets);
    ys.Update(pby.GetData(), prmblock_offsets);

    MultPrmBlocked(xsv,adv,xs,ys);

    for (int s = 0; s < prmfes.Size(); s++)
    {
       if (cPprm[s])
       {
          cPprm[s]->MultTranspose(pby.GetBlock(s), by.GetBlock(s));
       }
       by.GetBlock(s).SetSubVector(*prmess_tdofs[s], 0.0);
    }
}


void ParametricBNLForm::Mult(const Vector &x, Vector &y) const
{

   BlockVector bx(x.GetData(), block_trueOffsets);
   BlockVector by(y.GetData(), block_trueOffsets);

   const BlockVector &pbx = Prolongate(bx);

   if (needs_prolongation)
   {
      aux2.Update(block_offsets);
   }
   BlockVector &pby = needs_prolongation ? aux2 : by;

   xs.Update(pbx.GetData(), block_offsets);
   ys.Update(pby.GetData(), block_offsets);
   MultBlocked(xs,xdv,ys);

   for (int s = 0; s < fes.Size(); s++)
   {
      if (cP[s])
      {
         cP[s]->MultTranspose(pby.GetBlock(s), by.GetBlock(s));
      }
      by.GetBlock(s).SetSubVector(*ess_tdofs[s], 0.0);
   }
}

void ParametricBNLForm::ComputeGradientBlocked(const BlockVector &bx, const BlockVector &dx) const
{
    const int skip_zeros = 0;
    Array<Array<int> *> vdofs(fes.Size());
    Array<Array<int> *> vdofs2(fes.Size());
    Array<Vector *> el_x(fes.Size());
    Array<const Vector *> el_x_const(fes.Size());
    Array2D<DenseMatrix *> elmats(fes.Size(), fes.Size());
    Array<const FiniteElement *>fe(fes.Size());
    Array<const FiniteElement *>fe2(fes.Size());

    ElementTransformation * T;

    Array<Array<int> *> prmvdofs(prmfes.Size());
    Array<Array<int> *> prmvdofs2(prmfes.Size());
    Array<Vector *> prmel_x(prmfes.Size());
    Array<const Vector *> prmel_x_const(prmfes.Size());
    Array<const FiniteElement *>prmfe(prmfes.Size());
    Array<const FiniteElement *>prmfe2(prmfes.Size());

    for (int i=0; i<fes.Size(); ++i)
    {
       el_x_const[i] = el_x[i] = new Vector();
       vdofs[i] = new Array<int>;
       vdofs2[i] = new Array<int>;
       for (int j=0; j<fes.Size(); ++j)
       {
          elmats(i,j) = new DenseMatrix();
       }
    }

    for (int i=0; i<fes.Size(); ++i)
    {
       for (int j=0; j<fes.Size(); ++j)
       {
          if (Grads(i,j) != NULL)
          {
             *Grads(i,j) = 0.0;
          }
          else
          {
             Grads(i,j) = new SparseMatrix(fes[i]->GetVSize(),
                                           fes[j]->GetVSize());
          }
       }
    }

    for (int i=0; i<prmfes.Size(); ++i)
    {
       prmel_x_const[i] = prmel_x[i] = new Vector();
       prmvdofs[i] = new Array<int>;
       prmvdofs2[i] = new Array<int>;
    }

    if (dnfi.Size())
    {
       for (int i = 0; i < fes[0]->GetNE(); ++i)
       {
          T = fes[0]->GetElementTransformation(i);

          for (int s = 0; s < fes.Size(); ++s)
          {
             fe[s] = fes[s]->GetFE(i);
             fes[s]->GetElementVDofs(i, *vdofs[s]);
             bx.GetBlock(s).GetSubVector(*vdofs[s], *el_x[s]);
          }

          for (int s = 0; s < prmfes.Size(); ++s)
          {
             prmfe[s] = prmfes[s]->GetFE(i);
             prmfes[s]->GetElementVDofs(i, *prmvdofs[s]);
             dx.GetBlock(s).GetSubVector(*prmvdofs[s], *prmel_x[s]);
          }

          for (int k = 0; k < dnfi.Size(); ++k)
          {
             dnfi[k]->AssembleElementGrad(fe,prmfe,*T, el_x_const, prmel_x_const, elmats);

             for (int j=0; j<fes.Size(); ++j)
             {
                for (int l=0; l<fes.Size(); ++l)
                {
                   if (elmats(j,l)->Height() == 0) { continue; }
                   Grads(j,l)->AddSubMatrix(*vdofs[j], *vdofs[l],
                                            *elmats(j,l), skip_zeros);
                }
             }
          }
       }
    }

    if (fnfi.Size())
    {
       FaceElementTransformations *tr;
       Mesh *mesh = fes[0]->GetMesh();

       for (int i = 0; i < mesh->GetNumFaces(); ++i)
       {
          tr = mesh->GetInteriorFaceTransformations(i);

          for (int s=0; s < fes.Size(); ++s)
          {
             fe[s] = fes[s]->GetFE(tr->Elem1No);
             fe2[s] = fes[s]->GetFE(tr->Elem2No);

             fes[s]->GetElementVDofs(tr->Elem1No, *vdofs[s]);
             fes[s]->GetElementVDofs(tr->Elem2No, *vdofs2[s]);
             vdofs[s]->Append(*(vdofs2[s]));

             bx.GetBlock(s).GetSubVector(*vdofs[s], *el_x[s]);
          }

          for (int s=0; s < prmfes.Size(); ++s)
          {
             prmfe[s] = prmfes[s]->GetFE(tr->Elem1No);
             prmfe2[s] = prmfes[s]->GetFE(tr->Elem2No);

             prmfes[s]->GetElementVDofs(tr->Elem1No, *prmvdofs[s]);
             prmfes[s]->GetElementVDofs(tr->Elem2No, *prmvdofs2[s]);
             prmvdofs[s]->Append(*(prmvdofs2[s]));

             dx.GetBlock(s).GetSubVector(*prmvdofs[s], *prmel_x[s]);
          }



          for (int k = 0; k < fnfi.Size(); ++k)
          {
             fnfi[k]->AssembleFaceGrad(fe, fe2, prmfe, prmfe2, *tr, el_x_const, prmel_x_const, elmats);
             for (int j=0; j<fes.Size(); ++j)
             {
                for (int l=0; l<fes.Size(); ++l)
                {
                   if (elmats(j,l)->Height() == 0) { continue; }
                   Grads(j,l)->AddSubMatrix(*vdofs[j], *vdofs[l],
                                            *elmats(j,l), skip_zeros);
                }
             }
          }
       }
    }

    if (bfnfi.Size())
    {
       FaceElementTransformations *tr;
       Mesh *mesh = fes[0]->GetMesh();

       // Which boundary attributes need to be processed?
       Array<int> bdr_attr_marker(mesh->bdr_attributes.Size() ?
                                  mesh->bdr_attributes.Max() : 0);
       bdr_attr_marker = 0;
       for (int k = 0; k < bfnfi.Size(); ++k)
       {
          if (bfnfi_marker[k] == NULL)
          {
             bdr_attr_marker = 1;
             break;
          }
          Array<int> &bdr_marker = *bfnfi_marker[k];
          MFEM_ASSERT(bdr_marker.Size() == bdr_attr_marker.Size(),
                      "invalid boundary marker for boundary face integrator #"
                      << k << ", counting from zero");
          for (int i = 0; i < bdr_attr_marker.Size(); ++i)
          {
             bdr_attr_marker[i] |= bdr_marker[i];
          }
       }

       for (int i = 0; i < mesh->GetNBE(); ++i)
       {
          const int bdr_attr = mesh->GetBdrAttribute(i);
          if (bdr_attr_marker[bdr_attr-1] == 0) { continue; }

          tr = mesh->GetBdrFaceTransformations(i);
          if (tr != NULL)
          {
             for (int s = 0; s < fes.Size(); ++s)
             {
                fe[s] = fes[s]->GetFE(tr->Elem1No);
                fe2[s] = fe[s];

                fes[s]->GetElementVDofs(i, *vdofs[s]);
                bx.GetBlock(s).GetSubVector(*vdofs[s], *el_x[s]);
             }

             for (int s = 0; s < prmfes.Size(); ++s)
             {
                prmfe[s] = prmfes[s]->GetFE(tr->Elem1No);
                prmfe2[s] = prmfe[s];

                prmfes[s]->GetElementVDofs(i, *prmvdofs[s]);
                dx.GetBlock(s).GetSubVector(*prmvdofs[s], *prmel_x[s]);
             }


             for (int k = 0; k < bfnfi.Size(); ++k)
             {
                bfnfi[k]->AssembleFaceGrad(fe, fe2, prmfe, prmfe2, *tr, el_x_const, prmel_x_const, elmats);
                for (int l=0; l<fes.Size(); ++l)
                {
                   for (int j=0; j<fes.Size(); ++j)
                   {
                      if (elmats(j,l)->Height() == 0) { continue; }
                      Grads(j,l)->AddSubMatrix(*vdofs[j], *vdofs[l],
                                               *elmats(j,l), skip_zeros);
                   }
                }
             }
          }
       }
    }

    if (!Grads(0,0)->Finalized())
    {
       for (int i=0; i<fes.Size(); ++i)
       {
          for (int j=0; j<fes.Size(); ++j)
          {
             Grads(i,j)->Finalize(skip_zeros);
          }
       }
    }

    for (int i=0; i<fes.Size(); ++i)
    {
       for (int j=0; j<fes.Size(); ++j)
       {
          delete elmats(i,j);
       }
       delete vdofs2[i];
       delete vdofs[i];
       delete el_x[i];
    }

    for (int i=0; i<prmfes.Size(); ++i)
    {
       delete prmvdofs2[i];
       delete prmvdofs[i];
       delete prmel_x[i];
    }
}

BlockOperator& ParametricBNLForm::GetGradient(const Vector &x) const
{
    BlockVector bx(x.GetData(), block_trueOffsets);
    const BlockVector &pbx = Prolongate(bx);

    ComputeGradientBlocked(pbx, xdv);

    Array2D<SparseMatrix *> mGrads(fes.Size(), fes.Size());
    mGrads = Grads;
    if (needs_prolongation)
    {
       for (int s1 = 0; s1 < fes.Size(); ++s1)
       {
          for (int s2 = 0; s2 < fes.Size(); ++s2)
          {
             delete cGrads(s1, s2);
             cGrads(s1, s2) = RAP(*cP[s1], *Grads(s1, s2), *cP[s2]);
             mGrads(s1, s2) = cGrads(s1, s2);
          }
       }
    }

    for (int s = 0; s < fes.Size(); ++s)
    {
       for (int i = 0; i < ess_tdofs[s]->Size(); ++i)
       {
          for (int j = 0; j < fes.Size(); ++j)
          {
             if (s == j)
             {
                mGrads(s, s)->EliminateRowCol((*ess_tdofs[s])[i],
                                              Matrix::DIAG_ONE);
             }
             else
             {
                mGrads(s, j)->EliminateRow((*ess_tdofs[s])[i]);
                mGrads(j, s)->EliminateCol((*ess_tdofs[s])[i]);
             }
          }
       }
    }

    delete BlockGrad;
    BlockGrad = new BlockOperator(block_trueOffsets);
    for (int i = 0; i < fes.Size(); ++i)
    {
       for (int j = 0; j < fes.Size(); ++j)
       {
          BlockGrad->SetBlock(i, j, mGrads(i, j));
       }
    }
    return *BlockGrad;

}

ParametricBNLForm::~ParametricBNLForm()
{
    delete BlockGrad;
    for (int i=0; i<fes.Size(); ++i)
    {
       for (int j=0; j<fes.Size(); ++j)
       {
          delete Grads(i,j);
          delete cGrads(i,j);
       }
       delete ess_tdofs[i];
    }

    for (int i=0; i<prmfes.Size(); ++i)
    {
       delete prmess_tdofs[i];
    }

    for (int i = 0; i < dnfi.Size(); ++i)
    {
       delete dnfi[i];
    }

    for (int i = 0; i < fnfi.Size(); ++i)
    {
       delete fnfi[i];
    }

    for (int i = 0; i < bfnfi.Size(); ++i)
    {
       delete bfnfi[i];
    }

}

}


