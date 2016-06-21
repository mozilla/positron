# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import absolute_import, print_function, unicode_literals

import concurrent.futures as futures
import requests
import requests.adapters
import json
import collections
import os
import logging

from slugid import nice as slugid

logger = logging.getLogger(__name__)

def create_tasks(taskgraph, label_to_taskid):
    # TODO: use the taskGroupId of the decision task
    task_group_id = slugid()
    taskid_to_label = {t: l for l, t in label_to_taskid.iteritems()}

    session = requests.Session()

    decision_task_id = os.environ.get('TASK_ID')

    with futures.ThreadPoolExecutor(requests.adapters.DEFAULT_POOLSIZE) as e:
        fs = {}

        # We can't submit a task until its dependencies have been submitted.
        # So our strategy is to walk the graph and submit tasks once all
        # their dependencies have been submitted.
        #
        # Using visit_postorder() here isn't the most efficient: we'll
        # block waiting for dependencies of task N to submit even though
        # dependencies for task N+1 may be finished. If we need to optimize
        # this further, we can build a graph of task dependencies and walk
        # that.
        for task_id in taskgraph.graph.visit_postorder():
            task_def = taskgraph.tasks[task_id].task

            # if this task has no dependencies, make it depend on this decision
            # task so that it does not start immediately; and so that if this loop
            # fails halfway through, none of the already-created tasks run.
            if decision_task_id and not task_def.get('dependencies'):
                task_def['dependencies'] = [decision_task_id]

            task_def['taskGroupId'] = task_group_id

            # Wait for dependencies before submitting this.
            deps_fs = [fs[dep] for dep in task_def['dependencies'] if dep in fs]
            for f in futures.as_completed(deps_fs):
                f.result()

            fs[task_id] = e.submit(_create_task, session, task_id,
                                   taskid_to_label[task_id], task_def)

        # Wait for all futures to complete.
        for f in futures.as_completed(fs.values()):
            f.result()

def _create_task(session, task_id, label, task_def):
    # create the task using 'http://taskcluster/queue', which is proxied to the queue service
    # with credentials appropriate to this job.
    logger.debug("Creating task with taskId {} for {}".format(task_id, label))
    res = session.put('http://taskcluster/queue/v1/task/{}'.format(task_id), data=json.dumps(task_def))
    if res.status_code != 200:
        try:
            logger.error(res.json()['message'])
        except:
            logger.error(res.text)
        res.raise_for_status()
