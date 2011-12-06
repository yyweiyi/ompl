/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ioan Sucan */

#include "ompl/geometric/planners/est/EST.h"
#include "ompl/base/GoalSampleableRegion.h"
#include "ompl/tools/config/SelfConfig.h"
#include <limits>
#include <cassert>

ompl::geometric::EST::EST(const base::SpaceInformationPtr &si) : base::Planner(si, "EST")
{
    specs_.approximateSolutions = true;
    goalBias_ = 0.05;
    maxDistance_ = 0.0;

    Planner::declareParam<double>("range", this, &EST::setRange, &EST::getRange);
    Planner::declareParam<double>("goal_bias", this, &EST::setGoalBias, &EST::getGoalBias);
}

ompl::geometric::EST::~EST(void)
{
    freeMemory();
}

void ompl::geometric::EST::setup(void)
{
    Planner::setup();
    SelfConfig sc(si_, getName());
    sc.configureProjectionEvaluator(projectionEvaluator_);
    sc.configurePlannerRange(maxDistance_);

    tree_.grid.setDimension(projectionEvaluator_->getDimension());
}

void ompl::geometric::EST::clear(void)
{
    Planner::clear();
    sampler_.reset();
    freeMemory();
    tree_.grid.clear();
    tree_.size = 0;
}

void ompl::geometric::EST::freeMemory(void)
{
    for (Grid<MotionSet>::iterator it = tree_.grid.begin(); it != tree_.grid.end() ; ++it)
    {
        for (unsigned int i = 0 ; i < it->second->data.size() ; ++i)
        {
            if (it->second->data[i]->state)
                si_->freeState(it->second->data[i]->state);
            delete it->second->data[i];
        }
    }
}

bool ompl::geometric::EST::solve(const base::PlannerTerminationCondition &ptc)
{
    checkValidity();
    base::Goal                   *goal = pdef_->getGoal().get();
    base::GoalSampleableRegion *goal_s = dynamic_cast<base::GoalSampleableRegion*>(goal);

    while (const base::State *st = pis_.nextStart())
    {
        Motion *motion = new Motion(si_);
        si_->copyState(motion->state, st);
        addMotion(motion);
    }

    if (tree_.grid.size() == 0)
    {
        msg_.error("There are no valid initial states!");
        return false;
    }

    if (!sampler_)
        sampler_ = si_->allocValidStateSampler();

    msg_.inform("Starting with %u states", tree_.size);

    Motion *solution  = NULL;
    Motion *approxsol = NULL;
    double  approxdif = std::numeric_limits<double>::infinity();
    base::State *xstate = si_->allocState();

    while (ptc() == false)
    {
        /* Decide on a state to expand from */
        Motion *existing = selectMotion();
        assert(existing);

        /* sample random state (with goal biasing) */
        if (goal_s && rng_.uniform01() < goalBias_ && goal_s->canSample())
            goal_s->sampleGoal(xstate);
        else
            if (!sampler_->sampleNear(xstate, existing->state, maxDistance_))
                continue;

        if (si_->checkMotion(existing->state, xstate))
        {
            /* create a motion */
            Motion *motion = new Motion(si_);
            si_->copyState(motion->state, xstate);
            motion->parent = existing;

            addMotion(motion);
            double dist = 0.0;
            bool solved = goal->isSatisfied(motion->state, &dist);
            if (solved)
            {
                approxdif = dist;
                solution = motion;
                break;
            }
            if (dist < approxdif)
            {
                approxdif = dist;
                approxsol = motion;
            }
        }
    }

    bool solved = false;
    bool approximate = false;
    if (solution == NULL)
    {
        solution = approxsol;
        approximate = true;
    }

    if (solution != NULL)
    {
        /* construct the solution path */
        std::vector<Motion*> mpath;
        while (solution != NULL)
        {
            mpath.push_back(solution);
            solution = solution->parent;
        }

        /* set the solution path */
        PathGeometric *path = new PathGeometric(si_);
        for (int i = mpath.size() - 1 ; i >= 0 ; --i)
            path->states.push_back(si_->cloneState(mpath[i]->state));
        goal->addSolutionPath(base::PathPtr(path), approximate, approxdif);
        solved = true;
    }

    si_->freeState(xstate);

    msg_.inform("Created %u states in %u cells", tree_.size, tree_.grid.size());

    return solved;
}

ompl::geometric::EST::Motion* ompl::geometric::EST::selectMotion(void)
{
    GridCell* cell = pdf_.sample(rng_.uniform01());
    return cell && !cell->data.empty() ? cell->data[rng_.uniformInt(0, cell->data.size() - 1)] : NULL;
}

void ompl::geometric::EST::addMotion(Motion *motion)
{
    Grid<MotionSet>::Coord coord;
    projectionEvaluator_->computeCoordinates(motion->state, coord);
    GridCell* cell = tree_.grid.getCell(coord);
    if (cell)
    {
        cell->data.push_back(motion);
        pdf_.update(cellToElem_[cell], 1.0/cell->data.size());
    }
    else
    {
        cell = tree_.grid.createCell(coord);
        cell->data.push_back(motion);
        tree_.grid.add(cell);
        cellToElem_[cell] = pdf_.add(cell, 1.0);
    }
    tree_.size++;
}

void ompl::geometric::EST::getPlannerData(base::PlannerData &data) const
{
    Planner::getPlannerData(data);

    std::vector<MotionSet> motions;
    tree_.grid.getContent(motions);

    for (unsigned int i = 0 ; i < motions.size() ; ++i)
        for (unsigned int j = 0 ; j < motions[i].size() ; ++j)
            data.recordEdge(motions[i][j]->parent ? motions[i][j]->parent->state : NULL, motions[i][j]->state);
}
