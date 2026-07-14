"""Thin wrapper over the k8s Python client.

Creates + deletes Pods, Services, and Ingresses for game sessions.
Runs in-cluster (uses ServiceAccount token in
/var/run/secrets/kubernetes.io/serviceaccount) OR locally (uses
~/.kube/config); tries in-cluster first.

Naming scheme:
  game_id     = "g-<8 chars>"
  pod_name    = game_id + "-pod"
  service_name= game_id + "-svc"
  ingress_name= game_id + "-ing"

All resources land in `settings.games_namespace` (default:
`simsc-games`).
"""
from __future__ import annotations

import logging
import secrets
from dataclasses import dataclass

from kubernetes import client, config
from kubernetes.client.rest import ApiException

from app.core.config import settings

log = logging.getLogger("simsc.k8s")

_initialized = False


def _init() -> None:
    global _initialized
    if _initialized:
        return
    try:
        config.load_incluster_config()
        log.info("k8s: in-cluster config loaded")
    except config.ConfigException:
        config.load_kube_config()
        log.info("k8s: kubeconfig loaded (dev mode)")
    _initialized = True


def make_game_id() -> str:
    return "g-" + secrets.token_hex(4)


@dataclass
class GameHandles:
    game_id: str
    pod_name: str
    service_name: str
    ingress_name: str


def create_game(
    game_id: str,
    map_name: str,
    races: list[str],
    user_hashes: list[tuple[str, str, int]],
    game_speed: str = "fastest",
) -> GameHandles:
    """Create Pod + Service + Ingress for one game.

    Args:
      user_hashes: list of (alias, sha256hex, slot). One entry per
                   (player, active key). Multiple entries with the
                   same alias/slot are OK — they bind multiple keys
                   to the same player identity. Passed as repeated
                   `--user-hash alias:sha256hex:player:slot` args.
      game_speed: name of a --game-speed preset. See openbw_server
                  --help for the list; validated at the API layer
                  (`services.games.GAME_SPEEDS`).
    """
    _init()
    pod_name = f"{game_id}-pod"
    svc_name = f"{game_id}-svc"
    ing_name = f"{game_id}-ing"
    ns = settings.games_namespace
    label = {"app": "openbw-game", "game-id": game_id}

    core = client.CoreV1Api()
    net = client.NetworkingV1Api()

    # --- args ---
    args = [
        "--map", f"/opt/openbw/data/{map_name}",
        "--game-speed", game_speed,
        "--any-ws-path",  # ALB path-routes to us; accept any WS path.
    ]
    for i, race in enumerate(races):
        # Control server resolves 'random' → concrete race before
        # calling us (see games.py:_resolve_races), so every slot
        # should carry one of zerg/terran/protoss. The `in` guard
        # is belt-and-suspenders for older DB rows.
        if race in ("zerg", "terran", "protoss"):
            args.extend(["--race", f"{i}={race}"])
    for alias, hex_hash, slot in user_hashes:
        args.extend(["--user-hash", f"{alias}:{hex_hash}:player:{slot}"])

    # --- Pod ---
    pod = client.V1Pod(
        metadata=client.V1ObjectMeta(name=pod_name, namespace=ns, labels=label),
        spec=client.V1PodSpec(
            restart_policy="Never",
            containers=[
                client.V1Container(
                    name="openbw-server",
                    image=settings.openbw_server_image,
                    args=args,
                    ports=[
                        client.V1ContainerPort(container_port=6113, name="agent"),
                        client.V1ContainerPort(container_port=6114, name="observer"),
                    ],
                    resources=client.V1ResourceRequirements(
                        requests={"cpu": "200m", "memory": "256Mi"},
                        limits={"cpu": "1000m", "memory": "1Gi"},
                    ),
                ),
            ],
        ),
    )
    core.create_namespaced_pod(namespace=ns, body=pod)

    # --- Service (ClusterIP; ALB targets IP-mode via TargetGroupBinding) ---
    svc = client.V1Service(
        metadata=client.V1ObjectMeta(name=svc_name, namespace=ns, labels=label),
        spec=client.V1ServiceSpec(
            type="ClusterIP",
            selector=label,
            ports=[
                client.V1ServicePort(name="agent", port=6113, target_port=6113),
                client.V1ServicePort(name="observer", port=6114, target_port=6114),
            ],
        ),
    )
    core.create_namespaced_service(namespace=ns, body=svc)

    # --- Ingress (path-per-game; joins the shared 'simsc' ALB group) ---
    ing = client.V1Ingress(
        metadata=client.V1ObjectMeta(
            name=ing_name,
            namespace=ns,
            labels=label,
            annotations={
                "kubernetes.io/ingress.class": "alb",
                "alb.ingress.kubernetes.io/group.name": "simsc",
                # Priority between simsc-public (10) and simsc-cognito (100).
                # Games get 50, so /game/{id}/* matches before /* catch-all.
                "alb.ingress.kubernetes.io/group.order": "50",
                "alb.ingress.kubernetes.io/scheme": "internet-facing",
                "alb.ingress.kubernetes.io/target-type": "ip",
                "alb.ingress.kubernetes.io/listen-ports": '[{"HTTP":80},{"HTTPS":443}]',
                "alb.ingress.kubernetes.io/ssl-redirect": "443",
                "alb.ingress.kubernetes.io/certificate-arn": settings.games_acm_cert_arn,
                # Tag values here must match the other ingresses in
                # this ALB group ('simsc'), otherwise the ALB Ingress
                # Controller refuses to reconcile — the entire group
                # (main SPA + all games) sits with stale ALB rules.
                # The main simsc ingress uses environment=m2; keep
                # this the same value.
                "alb.ingress.kubernetes.io/tags":
                    f"project=simsc,environment=m2,game-id={game_id}",
            },
        ),
        spec=client.V1IngressSpec(
            ingress_class_name="alb",
            rules=[
                client.V1IngressRule(
                    host=settings.games_host,
                    http=client.V1HTTPIngressRuleValue(paths=[
                        client.V1HTTPIngressPath(
                            path=f"/game/{game_id}/agent",
                            path_type="Prefix",
                            backend=client.V1IngressBackend(
                                service=client.V1IngressServiceBackend(
                                    name=svc_name,
                                    port=client.V1ServiceBackendPort(number=6113),
                                ),
                            ),
                        ),
                        client.V1HTTPIngressPath(
                            path=f"/game/{game_id}/observer",
                            path_type="Prefix",
                            backend=client.V1IngressBackend(
                                service=client.V1IngressServiceBackend(
                                    name=svc_name,
                                    port=client.V1ServiceBackendPort(number=6114),
                                ),
                            ),
                        ),
                    ]),
                ),
            ],
        ),
    )
    net.create_namespaced_ingress(namespace=ns, body=ing)

    return GameHandles(
        game_id=game_id,
        pod_name=pod_name,
        service_name=svc_name,
        ingress_name=ing_name,
    )


def delete_game(handles: GameHandles) -> None:
    _init()
    core = client.CoreV1Api()
    net = client.NetworkingV1Api()
    ns = settings.games_namespace
    for delete_fn, name in (
        (lambda: net.delete_namespaced_ingress(handles.ingress_name, ns), "ingress"),
        (lambda: core.delete_namespaced_service(handles.service_name, ns), "service"),
        (lambda: core.delete_namespaced_pod(handles.pod_name, ns), "pod"),
    ):
        try:
            delete_fn()
        except ApiException as e:
            if e.status != 404:
                log.warning("k8s delete %s failed: %s", name, e)


def pod_phase(pod_name: str) -> str:
    _init()
    ns = settings.games_namespace
    try:
        pod = client.CoreV1Api().read_namespaced_pod(pod_name, ns)
        return pod.status.phase or "Unknown"
    except ApiException as e:
        if e.status == 404:
            return "Gone"
        raise
