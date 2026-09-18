pipeline {
  agent any

  options {
    disableConcurrentBuilds()
    timestamps()
  }

  stages {
    stage('Deploy API to EC2') {
      steps {
        sshagent(credentials: ['vampire-ec2-deploy']) {
          sh '''
            set -eu

            ssh \
              -o BatchMode=yes \
              -o ConnectTimeout=15 \
              -o StrictHostKeyChecking=yes \
              ubuntu@43.200.76.18 \
              'cd /home/ubuntu/deploy && exec /home/ubuntu/bin/deploy-vampire-core.sh'
          '''
        }
      }
    }
  }
}
