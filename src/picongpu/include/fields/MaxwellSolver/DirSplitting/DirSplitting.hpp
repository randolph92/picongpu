/* Copyright 2013-2017 Axel Huebl, Heiko Burau, Rene Widera
 *
 * This file is part of PIConGPU.
 *
 * PIConGPU is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PIConGPU is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PIConGPU.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "DirSplitting.def"

#include "simulation_defines.hpp"

#include "fields/MaxwellSolver/DirSplitting/DirSplitting.kernel"
#include "dataManagement/DataConnector.hpp"
#include "fields/FieldB.hpp"
#include "fields/FieldE.hpp"
#include "lambda/Expression.hpp"
#include "cuSTL/algorithm/kernel/ForeachBlock.hpp"
#include "cuSTL/cursor/NestedCursor.hpp"
#include "math/Vector.hpp"
#include "math/vector/Int.hpp"
#include "math/vector/TwistComponents.hpp"
#include "math/vector/compile-time/TwistComponents.hpp"


namespace picongpu
{
namespace dirSplitting
{
using namespace PMacc;

/** Check Directional Splitting grid and time conditions
 *
 * This is a workaround that the condition check is only
 * triggered if the current used solver is `DirSplitting`
 */
template<typename T_UsedSolver, typename T_Dummy=void>
struct ConditionCheck
{
};

template<typename T_Dummy>
struct ConditionCheck<DirSplitting, T_Dummy>
{
    /* Directional Splitting conditions:
     *
     * using SI units to avoid round off errors
     *
     * The compiler is allowed to evaluate an expression those not depends on a template parameter
     * even if the class is never instantiated. In that case static assert is always
     * evaluated (e.g. with clang), this results in an error if the condition is false.
     * http://www.boost.org/doc/libs/1_60_0/doc/html/boost_staticassert.html
     *
     * A workaround is to add a template dependency to the expression.
     * `sizeof(ANY_TYPE) != 0` is always true and defers the evaluation.
     */
    PMACC_CASSERT_MSG(DirectionSplitting_Set_dX_equal_dt_times_c____check_your_gridConfig_param_file,
                      (SI::SPEED_OF_LIGHT_SI * SI::DELTA_T_SI) == SI::CELL_WIDTH_SI &&
                      (sizeof(T_Dummy) != 0));
    PMACC_CASSERT_MSG(DirectionSplitting_use_cubic_cells____check_your_gridConfig_param_file,
                      SI::CELL_HEIGHT_SI == SI::CELL_WIDTH_SI &&
                      (sizeof(T_Dummy) != 0));
#if (SIMDIM == DIM3)
    PMACC_CASSERT_MSG(DirectionSplitting_use_cubic_cells____check_your_gridConfig_param_file,
                      SI::CELL_DEPTH_SI == SI::CELL_WIDTH_SI &&
                      (sizeof(T_Dummy) != 0));
#endif
};

class DirSplitting : private ConditionCheck<fieldSolver::FieldSolver>
{
private:
    template<typename OrientationTwist,typename CursorE, typename CursorB, typename GridSize>
    void propagate(CursorE cursorE, CursorB cursorB, GridSize gridSize) const
    {
        using namespace cursor::tools;
        using namespace PMacc::math;

        auto gridSizeTwisted = twistComponents<OrientationTwist>(gridSize);

        /* twist components of the supercell */
        typedef typename CT::TwistComponents<SuperCellSize, OrientationTwist>::type BlockDim;

        algorithm::kernel::ForeachBlock<BlockDim> foreach;
        foreach(zone::SphericZone<3>(PMacc::math::Size_t<3>(BlockDim::x::value, gridSizeTwisted.y(), gridSizeTwisted.z())),
                cursor::make_NestedCursor(twistVectorFieldAxes<OrientationTwist>(cursorE)),
                cursor::make_NestedCursor(twistVectorFieldAxes<OrientationTwist>(cursorB)),
                DirSplittingKernel<BlockDim>((int)gridSizeTwisted.x()));
    }
public:
    DirSplitting(MappingDesc) {}

    void update_beforeCurrent(uint32_t currentStep) const
    {
        typedef SuperCellSize GuardDim;

        DataConnector &dc = Environment<>::get().DataConnector();

        auto fieldE = dc.get< FieldE >( FieldE::getName(), true );
        auto fieldB = dc.get< FieldB >( FieldB::getName(), true );

        auto fieldE_coreBorder =
            fieldE->getGridBuffer().getDeviceBuffer().
                   cartBuffer().view(GuardDim().toRT(),
                                     -GuardDim().toRT());
        auto fieldB_coreBorder =
            fieldB->getGridBuffer().getDeviceBuffer().
            cartBuffer().view(GuardDim().toRT(),
                              -GuardDim().toRT());

        using namespace cursor::tools;
        using namespace PMacc::math;

        PMacc::math::Size_t<3> gridSize = fieldE_coreBorder.size();


        typedef PMacc::math::CT::Int<0,1,2> Orientation_X;
        propagate<Orientation_X>(
                  fieldE_coreBorder.origin(),
                  fieldB_coreBorder.origin(),
                  gridSize);

        __setTransactionEvent(fieldE->asyncCommunication(__getTransactionEvent()));
        __setTransactionEvent(fieldB->asyncCommunication(__getTransactionEvent()));

        typedef PMacc::math::CT::Int<1,2,0> Orientation_Y;
        propagate<Orientation_Y>(
                  fieldE_coreBorder.origin(),
                  fieldB_coreBorder.origin(),
                  gridSize);

        __setTransactionEvent(fieldE->asyncCommunication(__getTransactionEvent()));
        __setTransactionEvent(fieldB->asyncCommunication(__getTransactionEvent()));

        typedef PMacc::math::CT::Int<2,0,1> Orientation_Z;
        propagate<Orientation_Z>(
                  fieldE_coreBorder.origin(),
                  fieldB_coreBorder.origin(),
                  gridSize);

        if (laserProfile::INIT_TIME > float_X(0.0))
            fieldE->laserManipulation(currentStep);

        __setTransactionEvent(fieldE->asyncCommunication(__getTransactionEvent()));
        __setTransactionEvent(fieldB->asyncCommunication(__getTransactionEvent()));

        dc.releaseData( FieldE::getName() );
        dc.releaseData( FieldB::getName() );
    }

    void update_afterCurrent(uint32_t) const
    {
        DataConnector &dc = Environment<>::get().DataConnector();

        auto fieldE = dc.get< FieldE >( FieldE::getName(), true );
        auto fieldB = dc.get< FieldB >( FieldB::getName(), true );

        EventTask eRfieldE = fieldE->asyncCommunication(__getTransactionEvent());
        EventTask eRfieldB = fieldB->asyncCommunication(__getTransactionEvent());
        __setTransactionEvent(eRfieldE);
        __setTransactionEvent(eRfieldB);

        dc.releaseData( FieldE::getName() );
        dc.releaseData( FieldB::getName() );
    }

    static PMacc::traits::StringProperty getStringProperties()
    {
        PMacc::traits::StringProperty propList( "name", "DS" );
        return propList;
    }
};

} // dirSplitting

} // picongpu
