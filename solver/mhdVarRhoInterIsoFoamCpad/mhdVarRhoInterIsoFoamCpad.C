/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2017 OpenCFD Ltd.
     \\/     M anipulation  |
-------------------------------------------------------------------------------
                            | Copyright (C) 2011-2017 OpenFOAM Foundation
                isoAdvector | Copyright (C) 2016 DHI
              Modified work | Copyright (C) 2018 Johan Roenby
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

Application
    interIsoFoam

Group
    grpMultiphaseSolvers

Description
    Solver derived from interFoam for two incompressible, isothermal immiscible
    fluids using the isoAdvector phase-fraction based interface capturing
    approach, with optional mesh motion and mesh topology changes including
    adaptive re-meshing.

    Reference:
    \verbatim
        Roenby, J., Bredmose, H. and Jasak, H. (2016).
        A computational method for sharp interface advection
        Royal Society Open Science, 3
        doi 10.1098/rsos.160405
    \endverbatim

    isoAdvector code supplied by Johan Roenby, STROMNING (2018)

\*---------------------------------------------------------------------------*/

#include "fvCFD.H"
#include "dynamicFvMesh.H"
#include "isoAdvection.H"
#include "EulerDdtScheme.H"
#include "localEulerDdtScheme.H"
#include "CrankNicolsonDdtScheme.H"
#include "subCycle.H"
#include "immiscibleIncompressibleTwoPhaseMixture.H"
#include "varRhoTurbulentTransportModel.H"
#include "pimpleControl.H"
#include "fvOptions.H"
#include "CorrectPhi.H"
#include "fvcSmooth.H"
#include "Elmer.H"
#include "cpad.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

int main(int argc, char *argv[])
{
    argList::addNote
    (
        "Solver for two incompressible, isothermal immiscible fluids"
        " using isoAdvector phase-fraction based interface capturing.\n"
        "With optional mesh motion and mesh topology changes including"
        " adaptive re-meshing.\n"
        "The solver is derived from interFoam"
    );

    #include "postProcess.H"

    #include "addCheckCaseOptions.H"
    #include "setRootCaseLists.H"
    #include "createTime.H"
    #include "createDynamicFvMesh.H"
    #include "initContinuityErrs.H"
    #include "createDyMControls.H"
    #include "createFields.H"
    #include "initCorrectPhi.H"
    #include "createUfIfPresent.H"

    turbulence->validate();

    #include "CourantNo.H"
    #include "setInitialDeltaT.H"

    // * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //
    Info<< "\nStarting time loop\n" << endl;

    // Info << "elcond_melt = "<< elcond_melt << endl;
    

    // Send fields to Elmer
    Elmer<dynamicFvMesh> sending(mesh,1); // 1=send, -1=receive
    sending.sendStatus(1); // 1=ok, 0=lastIter, -1=error

    // elcond = alpha1 * elcond_melt; // old
    // Start modified ----------
    elcond = alpha1 * elCondListByPhase[0] + (1 - alpha1) * elCondListByPhase[1];
    // End modified ----------
    sending.sendScalar(elcond);

    // Receive fields from Elmer
    Elmer<dynamicFvMesh> receiving(mesh,-1); // 1=send, -1=receive
    receiving.sendStatus(1); // 1=ok, 0=lastIter, -1=error
    receiving.recvVector(JxB_recv);

    while (runTime.run())
    {
        //JxB = JxB_recv*alpha1; // Wieso?
        JxB = JxB_recv; // New // per vol or absoulte force?

        #include "readDyMControls.H"
        #include "CourantNo.H"
        #include "alphaCourantNo.H"
        #include "setDeltaT.H"

        ++runTime;

        Info<< "Time = " << runTime.timeName() << nl << endl;

        // --- Pressure-velocity PIMPLE corrector loop
        while (pimple.loop())
        {
            if (pimple.firstIter() || moveMeshOuterCorrectors)
            {
                mesh.update();

                if (mesh.changing())
                {

                    gh = (g & mesh.C()) - ghRef;
                    ghf = (g & mesh.Cf()) - ghRef;

                    MRF.update();

                    if (correctPhi)
                    {
                        // Calculate absolute flux
                        // from the mapped surface velocity
                        phi = mesh.Sf() & Uf();

                        #include "correctPhi.H"

                        // Make the flux relative to the mesh motion
                        fvc::makeRelative(phi, U);

                        mixture.correct();
                    }

                    if (checkMeshCourantNo)
                    {
                        #include "meshCourantNo.H"
                    }
                }
            }

            #include "alphaControls.H"
            #include "alphaEqnSubCycle.H"

            mixture.correct();

            if (pimple.frozenFlow())
            {
                continue;
            }

            #include "UEqn.H"

            // --- Pressure corrector loop
            while (pimple.correct())
            {
                #include "pEqn.H"
            }

            if (pimple.turbCorr())
            {
                turbulence->correct();
            }
        }

        runTime.write();
        runTime.printExecutionTime(Info);

        //CPAD
        #include "getLocalToGlobalFaceArray.h"

        FoamCpad::cpad cpas(mixture, runTime);
        cpas.appendTimeStepReport(runTime.value(), procFaceToGlobalFaceList);


         // Check whether we need to update electromagnetic stuff with Elmer
        scalar maxRelDiff_local = (max(mag(alpha_old - alpha1))).value();

        bool doElmer = false;

        if
        ( 
            maxRelDiff_local>maxRelDiff 
            && (maxRelDiff<SMALL || maxRelDiff+SMALL<=1.0)
        ) 
        {
            doElmer = true;
            Info << "maxRelDifflocal = " << maxRelDiff_local << endl;
        }

        if(doElmer || !runTime.run()) 
        {
            Info << "FOAM: Continue Elmer!" << endl;
            alpha_old = alpha1;

            // Send fields to Elmer
            sending.sendStatus(runTime.run());

            // elcond = alpha1 * elcond_melt; // old

            // Start modified ----------
            elcond = alpha1 * elCondListByPhase[0] 
                     + (1 - alpha1) * elCondListByPhase[1];
            // End modified ----------
            sending.sendScalar(elcond);

            // Receive fields form Elmer
            receiving.sendStatus(runTime.run());
            receiving.recvVector(JxB_recv);
        }
    }

    Info<< "End\n" << endl;

    return 0;
}


// ************************************************************************* //
